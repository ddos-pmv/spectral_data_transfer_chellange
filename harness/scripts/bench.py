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

HARNESS = Path(__file__).resolve().parent.parent
SHM_TX = "/fanout_ring"
SHM_RX = "/fanout_rx_{}"
PCTS = [("p50", 0.50), ("p99", 0.99), ("p99.9", 0.999), ("p99.99", 0.9999)]


def listify(v):
    return v if isinstance(v, list) else [v]


def load(path):
    with open(path, "rb") as f:
        cfg = tomllib.load(f)
    # Свипуемые ключи приводим к спискам, дальше по коду разницы уже нет.
    cfg["bench"]["fanout"] = listify(cfg["bench"].get("fanout", 1))
    cfg["producer"]["rate"] = listify(cfg["producer"]["rate"])
    return cfg


def expand(spec):
    """"2-4,6" -> [2, 3, 4, 6]"""
    out = []
    for part in filter(None, spec.split(",")):
        lo, _, hi = part.partition("-")
        out += range(int(lo), int(hi or lo) + 1)
    return out


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


def spawn(argv, cpu, log):
    """taskset не даёт планировщику мигрировать спиннер, setarch -R выключает
    ASLR, чтобы раскладка кода не менялась от прогона к прогону."""
    pre = ["taskset", "-c", str(cpu)] if cpu is not None else []
    with log.open("w") as f:  # Popen дублирует fd, наш можно сразу закрыть
        return subprocess.Popen(pre + ["setarch", "-R"] + argv, cwd=HARNESS,
                                stdout=f, stderr=subprocess.STDOUT)


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


def stop(procs):
    for p in procs:
        p.terminate()
    for p in procs:
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill()


def rm_shm():
    for p in Path("/dev/shm").glob("fanout_*"):
        p.unlink(missing_ok=True)


def one_point(cfg, rate, fanout, rundir, tag):
    """Один прогон. Возвращает [(номер получателя, csv, лог)] по успешным."""
    slots, batch = str(cfg["producer"]["slots"]), str(cfg["sender"]["batch"])
    rx = cfg["receiver"][:fanout]
    procs, waiting = [], []
    rm_shm()
    try:
        # Порядок старта диктует то, кто создаёт сегмент: receiver'ы делают свои
        # rx-кольца и биндят порты, producer — tx-кольцо, остальные только цепляются.
        for i, r in enumerate(rx, 1):
            shm = SHM_RX.format(i)
            p = spawn(["./bin/receiver", "--listen", str(r["port"]), "--shm", shm,
                       "--slots", slots, "--batch", batch],
                      r.get("cpu"), rundir / f"{tag}-receiver-{i}.log")
            procs.append(p)
            wait_shm(shm, p)

        p = spawn(["./bin/producer", "--shm", SHM_TX, "--slots", slots, "--count", "0",
                   "--rate", str(rate), "--type", cfg["producer"]["type"]],
                  cfg["producer"].get("cpu"), rundir / f"{tag}-producer.log")
        procs.append(p)
        wait_shm(SHM_TX, p)

        dsts = [a for r in rx for a in ("--dst", f"127.0.0.1:{r['port']}")]
        procs.append(spawn(["./bin/sender", "--shm", SHM_TX, "--slots", slots,
                            "--from-edge", "--batch", batch] + dsts,
                           cfg["sender"].get("cpu"), rundir / f"{tag}-sender.log"))

        # Consumer'ы — единственные с условием останова: каждый пишет ровно
        # --count замеров. Всё выше по потоку крутится безлимитно и глушится
        # в finally, поэтому медленный старт не может оставить недобор.
        con = cfg["consumer"]
        for i, r in enumerate(rx, 1):
            csv = rundir / r.get("csv", f"{tag}-rx-{i}.csv")
            log = rundir / f"{tag}-rx-{i}.log"
            c = spawn(["./bin/consumer", "--shm", SHM_RX.format(i), "--slots", slots,
                       "--count", str(con["count"]), "--warmup", str(con["warmup"]),
                       "--from-edge", "--csv", str(csv)], r.get("consumer_cpu"), log)
            procs.append(c)
            waiting.append((i, c, csv, log))

        done = []
        for i, c, csv, log in waiting:
            rc = c.wait()
            if rc == 0:
                done.append((i, csv, log))
            else:
                # rc 3 = записано меньше --count: не тот прогон, о котором
                # просили, в сводку он попасть не должен.
                print(f"  получатель {i}: FAILED (rc={rc})")
                csv.unlink(missing_ok=True)
        return done
    finally:
        stop(procs)
        rm_shm()


def pct(v, p):
    """Nearest-rank, то же определение, что в metrics.h."""
    r = int(p * len(v))
    if r < p * len(v):
        r += 1
    return v[max(1, min(r, len(v))) - 1]


def read_result(csv, log):
    with csv.open() as f:
        next(f, None)
        lat = sorted(int(line.split(",")[1]) for line in f if line.strip())
    # drop_rate считает consumer по разрывам seq_id — всё, что транспорт потерял.
    drops = next((l.split(":")[1].strip() for l in log.read_text().splitlines()
                  if l.startswith("drop_rate")), "-")
    return lat, drops


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
                    f"git      : {git}\n")


def main():
    cfg_path = Path(sys.argv[1]) if len(sys.argv) > 1 else HARNESS / "scripts/bench.toml"
    cfg = load(cfg_path)
    check_isolation(cfg["bench"]["isolated"])
    build()

    rundir = HARNESS / "results" / datetime.now().strftime("%Y%m%d-%H%M%S")
    rundir.mkdir(parents=True)
    shutil.copy(cfg_path, rundir / "config.toml")
    env_snapshot(rundir / "env.txt")

    header = f"{'point':<22}{'rx':>3}{'n':>9}{'drops':>10}" + \
             "".join(f"{n:>11}" for n, _ in PCTS)
    print(header)
    rows = []
    for rate, fanout in itertools.product(cfg["producer"]["rate"], cfg["bench"]["fanout"]):
        if fanout > len(cfg["receiver"]):
            sys.exit(f"fanout {fanout} больше, чем секций [[receiver]] "
                     f"({len(cfg['receiver'])})")
        for rep in range(1, cfg["bench"]["repeats"] + 1):
            tag = f"r{rate}-f{fanout}-{rep}"
            for i, csv, log in one_point(cfg, rate, fanout, rundir, tag):
                lat, drops = read_result(csv, log)
                p = [pct(lat, q) for _, q in PCTS]
                rows.append(dict(zip(["rate", "fanout", "repeat", "receiver", "n",
                                      "drop_rate", *[n for n, _ in PCTS]],
                                     [rate, fanout, rep, i, len(lat), drops, *p]))
                            | {"csv": csv})
                print(f"{tag:<22}{i:>3}{len(lat):>9,}{drops:>10}" +
                      "".join(f"{v:>11,}" for v in p))

    if not rows:
        sys.exit(f"годных прогонов нет, смотрите логи в {rundir}")

    summary = rundir / "summary.csv"
    cols = ["rate", "fanout", "repeat", "receiver", "n", "drop_rate", *[n for n, _ in PCTS]]
    with summary.open("w") as f:
        f.write(",".join(cols) + "\n")
        for r in rows:
            f.write(",".join(str(r[c]) for c in cols) + "\n")

    # Ноутбук читает один CSV; отдаём ему репрезентативный, а не последний.
    rep_row = sorted(rows, key=lambda r: r["p99"])[len(rows) // 2]
    notebook_csv = HARNESS / "scripts/latencies.csv"
    shutil.copyfile(rep_row["csv"], notebook_csv)
    print(f"\n{len(rows)} строк в {summary}; {rep_row['csv'].name} -> {notebook_csv}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit("\nпрервано")  # процессы и сегменты уже сняты в finally
