#!/usr/bin/env python3
"""Стартер бенча: поднимает всю цепочку локально по TOML-конфигу.

    producer -> /fanout_ring (shm) -> sender ==UDP 127.0.0.1==> receiver
             -> /fanout_rx_N (shm) -> consumer -> CSV + метрики

    ./bench.py [config.toml]      # по умолчанию scripts/bench.toml

Всё, что настраивается, лежит в конфиге — флагов у скрипта нет намеренно:
копия конфига кладётся рядом с результатами, поэтому по каталогу результатов
всегда видно, чем именно они получены.
"""
import itertools
import os
import shutil
import subprocess
import sys
import time
import tomllib
from datetime import datetime
from pathlib import Path
from typing import NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
import profilers  # noqa: E402
import profreport  # noqa: E402
from profilers import expand  # noqa: E402

HARNESS = Path(__file__).resolve().parent.parent
SHM_TX = "/fanout_ring"
SHM_RX = "/fanout_rx_{}"
PCTS = {"p50": 0.50, "p99": 0.99, "p99.9": 0.999, "p99.99": 0.9999}
SUMMARY_COLS = ["rate", "fanout", "repeat", "receiver", "n", "drop_rate",
                *PCTS, "profiled"]


# --- подготовка ------------------------------------------------------------

def load(path):
    """Конфиг + дефолты [profile] + проверки, которые дешевле сделать до сборки."""
    def listify(v):
        return v if isinstance(v, list) else [v]

    with open(path, "rb") as f:
        cfg = tomllib.load(f)
    # Свипуемые ключи приводим к спискам, дальше по коду разницы уже нет.
    cfg["bench"]["fanout"] = listify(cfg["bench"].get("fanout", 1))
    cfg["producer"]["rate"] = listify(cfg["producer"]["rate"])
    if (most := max(cfg["bench"]["fanout"])) > len(cfg["receiver"]):
        sys.exit(f"fanout {most} больше, чем секций [[receiver]] "
                 f"({len(cfg['receiver'])})")
    return profilers.defaults(cfg)


def check_isolation(want):
    """isolcpus задаётся только параметром загрузки ядра, отсюда его не выставить."""
    have = expand(Path("/sys/devices/system/cpu/isolated").read_text().strip())
    if set(want) - set(have):
        print(f"warning: конфиг ждёт isolcpus={want}, у ядра {have or '(нет)'} — "
              f"добавьте isolcpus/nohz_full/rcu_nocbs в cmdline и перезагрузитесь")


def build():
    """Пересборка каждый раз: иначе меряется то, что лежало в bin/."""
    subprocess.run(["make", "-B", "-j", str(os.cpu_count())],
                   cwd=HARNESS, check=True, stdout=subprocess.DEVNULL)


def env_snapshot(path):
    def read(p):
        try:
            return Path(p).read_text().strip()
        except OSError:
            return "-"

    git = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=HARNESS,
                         capture_output=True, text=True).stdout.strip()
    cpu = next((l.split(":", 1)[1].strip() for l in read("/proc/cpuinfo").splitlines()
                if l.startswith("model name")), "-")
    path.write_text(f"cpu      : {cpu}\n"
                    f"cmdline  : {read('/proc/cmdline')}\n"
                    f"isolated : {read('/sys/devices/system/cpu/isolated') or '(нет)'}\n"
                    f"governor : {read('/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor')}\n"
                    f"no_turbo : {read('/sys/devices/system/cpu/intel_pstate/no_turbo')}\n"
                    f"git      : {git}\n",
                    # Явно, а не по локали: под LANG=C кодировка по умолчанию ASCII,
                    # и «(нет)» уронило бы прогон UnicodeEncodeError'ом ещё на старте.
                    encoding="utf-8")


# --- один прогон -----------------------------------------------------------

class Point(NamedTuple):
    """Точка свипа: комбинация параметров плюс номер повтора."""
    rate: int
    fanout: int
    rep: int

    @property
    def tag(self):
        """Префикс всех артефактов точки — и логов, и CSV, и профилей."""
        return f"r{self.rate}-f{self.fanout}-{self.rep}"


def rm_shm():
    for p in Path("/dev/shm").glob("fanout_*"):
        p.unlink(missing_ok=True)


def wait_shm(name, proc, timeout=10):
    """Сегмент ftruncate'ится сразу после shm_open, так что ненулевой размер
    означает, что его уже можно мапить: иначе читатель гоняется с создателем."""
    path = Path("/dev/shm") / name.lstrip("/")
    deadline = time.monotonic() + timeout
    while not (path.exists() and path.stat().st_size):
        if proc.poll() is not None:
            sys.exit(f"{name}: процесс умер на старте, см. лог в каталоге прогона")
        if time.monotonic() > deadline:
            sys.exit(f"{name}: сегмент не появился за {timeout} с")
        time.sleep(0.02)


class Chain:
    """Процессы одной точки: запуск, профайлеры на них, гарантированный останов.

    Контекстный менеджер, потому что глушить надо в любом случае: всё выше
    consumer'ов по потоку крутится безлимитно, а брошенные сегменты /dev/shm
    подхватила бы следующая точка.
    """

    def __init__(self, cfg, rundir, point):
        self.cfg, self.rundir, self.point = cfg, rundir, point
        self.procs, self.profs, self.cpus = [], [], set()
        self.profile_roles = profilers.active_role(cfg, point.rep)

    def __enter__(self):
        rm_shm()
        return self

    def __exit__(self, *exc):
        for p in self.procs:
            p.terminate()
        for p in self.procs:
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
        # Только после смерти целей: pid-scoped perf дописывает артефакт именно по ней.
        profilers.stop_all(self.profs)
        rm_shm()

    def log_path(self, name):
        return self.rundir / f"{self.point.tag}-{name}.log"

    def spawn(self, role, cpu, *argv, name=None):
        """taskset не даёт планировщику мигрировать спиннер, setarch -R выключает
        ASLR, чтобы раскладка кода не менялась от прогона к прогону.

        name — имя артефактов, если оно отличается от роли (receiver-1, rx-1)."""
        log = self.log_path(name or role)
        pre = ["taskset", "-c", str(cpu)] if cpu is not None else []
        with log.open("w") as f:  # Popen дублирует fd, наш можно сразу закрыть
            p = subprocess.Popen(pre + ["setarch", "-R"] + [str(a) for a in argv],
                                 cwd=HARNESS, stdout=f, stderr=subprocess.STDOUT)
        self.procs.append(p)
        if cpu is not None:
            self.cpus.add(cpu)
        # Профайлеры навешиваются attach'ем на уже запущенный pid, сам процесс от
        # этого не меняется ничем.
        if self.profile_roles:
            self.profs += profilers.attach_role(role, p.pid, cpu,
                                                self.rundir / "prof" / log.stem, self.cfg)
        return p

    def attach_shared(self):
        """c2c и mem — на точку целиком, по объединению её ядер: строку кэша делят
        producer с sender'ом и receiver с consumer'ом, по одному pid HITM не увидеть.
        Свой набор повторов: системный сбор вытесняет per-task события.

        Вызывать после старта всех процессов точки — ядра берутся из них."""
        if profilers.active_shared(self.cfg, self.point.rep):
            self.profs += profilers.attach_shared(
                sorted(self.cpus), self.rundir / "prof" / self.point.tag, self.cfg)


def one_point(cfg, rundir, point):
    """Один прогон. Возвращает ([(номер получателя, csv, лог)], сборщики)."""
    slots, batch = cfg["producer"]["slots"], cfg["sender"]["batch"]
    rx = cfg["receiver"][:point.fanout]
    with Chain(cfg, rundir, point) as chain:
        # Порядок старта диктует то, кто создаёт сегмент: receiver'ы делают свои
        # rx-кольца и биндят порты, producer — tx-кольцо, остальные только цепляются.
        for i, r in enumerate(rx, 1):
            shm = SHM_RX.format(i)
            p = chain.spawn("receiver", r.get("cpu"),
                            "./bin/receiver", "--listen", r["port"], "--shm", shm,
                            "--slots", slots, "--batch", batch, name=f"receiver-{i}")
            wait_shm(shm, p)

        p = chain.spawn("producer", cfg["producer"].get("cpu"),
                        "./bin/producer", "--shm", SHM_TX, "--slots", slots,
                        "--count", 0, "--rate", point.rate,
                        "--type", cfg["producer"]["type"])
        wait_shm(SHM_TX, p)

        dsts = [a for r in rx for a in ("--dst", f"127.0.0.1:{r['port']}")]
        chain.spawn("sender", cfg["sender"].get("cpu"),
                    "./bin/sender", "--shm", SHM_TX, "--slots", slots,
                    "--from-edge", "--batch", batch, *dsts)

        # Consumer'ы — единственные с условием останова: каждый пишет ровно
        # --count замеров. Всё выше по потоку крутится безлимитно и глушится на
        # выходе из with, поэтому медленный старт не может оставить недобор.
        con = cfg["consumer"]
        waiting = []
        for i, r in enumerate(rx, 1):
            csv = rundir / r.get("csv", f"{point.tag}-rx-{i}.csv")
            c = chain.spawn("consumer", r.get("consumer_cpu"),
                            "./bin/consumer", "--shm", SHM_RX.format(i), "--slots", slots,
                            "--count", con["count"], "--warmup", con["warmup"],
                            "--from-edge", "--csv", csv, name=f"rx-{i}")
            waiting.append((i, c, csv, chain.log_path(f"rx-{i}")))
        chain.attach_shared()

        done = []
        for i, c, csv, log in waiting:
            if rc := c.wait():
                # rc 3 = записано меньше --count: не тот прогон, о котором
                # просили, в сводку он попасть не должен.
                print(f"  получатель {i}: FAILED (rc={rc})")
                csv.unlink(missing_ok=True)
            else:
                done.append((i, csv, log))
        return done, chain.profs


# --- результаты ------------------------------------------------------------

def pct(v, p):
    """Nearest-rank, то же определение, что в metrics.h."""
    r = int(p * len(v))
    if r < p * len(v):
        r += 1
    return v[max(1, min(r, len(v))) - 1]


def read_result(csv, log):
    """CSV замеров + лог consumer'а -> (отсортированные латентности, drop_rate)."""
    with csv.open() as f:
        next(f, None)
        lat = sorted(int(line.split(",")[1]) for line in f if line.strip())
    # drop_rate считает consumer по разрывам seq_id — всё, что транспорт потерял.
    drops = next((l.split(":")[1].strip() for l in log.read_text().splitlines()
                  if l.startswith("drop_rate")), "-")
    return lat, drops


def print_row(point, rx, n, drops, pcts):
    """Единственное место, где заданы ширины колонок сводной таблицы."""
    print(f"{point:<22}{rx:>3}{n:>9}{drops:>10}" + "".join(f"{v:>11}" for v in pcts))


def sweep(cfg, rundir):
    """Все точки конфига по очереди. Возвращает (строки сводки, сборщики)."""
    rows, profs = [], []
    print_row("point", "rx", "n", "drops", PCTS)
    for rate, fanout in itertools.product(cfg["producer"]["rate"], cfg["bench"]["fanout"]):
        for rep in range(1, cfg["bench"]["repeats"] + 1):
            point = Point(rate, fanout, rep)
            done, point_profs = one_point(cfg, rundir, point)
            profs += point_profs
            for i, csv, log in done:
                lat, drops = read_result(csv, log)
                p = [pct(lat, q) for q in PCTS.values()]
                rows.append({"rate": rate, "fanout": fanout, "repeat": rep,
                             "receiver": i, "n": len(lat), "drop_rate": drops,
                             **dict(zip(PCTS, p)),
                             # Сэмплирование ложится именно в хвост, поэтому
                             # профилированные точки помечены: смешивать их с
                             # чистыми в одной серии нельзя.
                             "profiled": profilers.tools_used(cfg, rep),
                             "csv": csv, "tag": point.tag})
                print_row(point.tag, i, f"{len(lat):,}", drops, [f"{v:,}" for v in p])
    return rows, profs


def write_summary(rundir, rows):
    path = rundir / "summary.csv"
    with path.open("w") as f:
        f.write(",".join(SUMMARY_COLS) + "\n")
        for r in rows:
            f.write(",".join(str(r[c]) for c in SUMMARY_COLS) + "\n")
    return path


def export_for_notebook(rows):
    """Ноутбук читает один CSV; отдаём ему репрезентативный, а не последний, и
    только из чистых точек — под профайлером хвост уже не тот."""
    clean = [r for r in rows if r["profiled"] == "-"] or rows
    median = sorted(clean, key=lambda r: r["p99"])[len(clean) // 2]
    dst = HARNESS / "scripts/latencies.csv"
    shutil.copyfile(median["csv"], dst)
    return median["csv"], dst


def main():
    cfg_path = Path(sys.argv[1]) if len(sys.argv) > 1 else HARNESS / "scripts/bench.toml"
    cfg = load(cfg_path)
    check_isolation(cfg["bench"]["isolated"])
    build()

    rundir = HARNESS / "results" / datetime.now().strftime("%Y%m%d-%H%M%S")
    rundir.mkdir(parents=True)
    shutil.copy(cfg_path, rundir / "config.toml")
    env_snapshot(rundir / "env.txt")

    rows, profs = sweep(cfg, rundir)
    if not rows:
        sys.exit(f"годных прогонов нет, смотрите логи в {rundir}")

    summary = write_summary(rundir, rows)
    src, dst = export_for_notebook(rows)
    print(f"\n{len(rows)} строк в {summary}; {src.name} -> {dst}")

    if profs:
        print("\nразбираю профили (perf report грузит ядро, поэтому только сейчас)...")
        profilers.postprocess_all(profs)
        if index := profreport.build(rundir, rows):
            print(f"отчёт: {index}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit("\nпрервано")  # процессы и сегменты уже сняты на выходе из with
