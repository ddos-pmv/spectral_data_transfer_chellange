"""Профайлеры, навешиваемые на процессы прогона bench.py.

Всё построено вокруг attach: `perf ... -p <pid>` к уже запущенному процессу, а не
`perf ... -- ./bin/sender`. Обёртка не годится по двум причинам, обе проверены:
убийство perf не убивает потомка (спиннеры остаются висеть вместе с портами и shm),
а `perf stat` по SIGTERM не пишет вообще ничего. bench.py глушит процессы именно
SIGTERM, так что обёртка сломала бы и прогон, и сбор. При attach же SIGTERM цели
доводит perf до штатного завершения с полными артефактами, а spawn()/stop() в
bench.py остаются нетронутыми.

Сборщик пинится на collector_cpu (housekeeping-ядро), чтобы не отъедать такты у
busy-spin процесса: на ядре цели остаётся только NMI от сэмплирования.

Два вида привязки:

    по роли (stat, record, cache-miss)  -p <pid>   завершаются сами по смерти цели
    на точку (c2c, mem)                 -C <ядра>  глушатся отсюда после точки

c2c и mem не могут быть per-role. У `perf mem record` ключ -p занят под
--phys-data, а не --pid (сбор просто падает), а c2c имеет смысл только по обеим
сторонам строки кэша: shm-кольцо делят producer с sender'ом и receiver с
consumer'ом, attach к одному pid не покажет ни одного HITM.
"""
import subprocess
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent
VENDOR = SCRIPTS / "vendor"

# Счётчики, а не сэмплы: PMU считает в фоне без прерываний, поэтому stat можно
# держать включённым даже на замерных повторах.
STAT_HW = ["cycles", "instructions", "cache-references", "cache-misses",
           "LLC-load-misses", "branch-misses", "dTLB-load-misses"]
STAT_SW = ["context-switches", "cpu-migrations", "page-faults"]

ROLES = ("producer", "sender", "receiver", "consumer")
ROLE_TOOLS = {"stat", "record", "cache-miss"}
SHARED_TOOLS = {"c2c", "mem"}


def expand(spec):
    """"2-4,6" -> [2, 3, 4, 6]"""
    out = []
    for part in filter(None, spec.split(",")):
        lo, _, hi = part.partition("-")
        out += range(int(lo), int(hi or lo) + 1)
    return out


def defaults(cfg):
    """Дефолты [profile] один раз здесь, чтобы дальше по коду не было .get()."""
    p = cfg.setdefault("profile", {})
    p.setdefault("enabled", False)
    p.setdefault("repeats", [])          # [] = все повторы
    p.setdefault("shared_repeats", [])
    p.setdefault("collector_cpu", 0)
    p.setdefault("freq", 997)
    p.setdefault("shared", [])
    for role in ("producer", "sender", "receiver", "consumer"):
        p.setdefault(role, {}).setdefault("tools", [])
    unknown = {t for role in ROLES for t in p[role]["tools"]} - ROLE_TOOLS
    unknown |= set(p["shared"]) - SHARED_TOOLS
    if unknown:
        raise SystemExit(f"[profile]: неизвестные инструменты {sorted(unknown)}; "
                         f"по ролям {sorted(ROLE_TOOLS)}, в shared {sorted(SHARED_TOOLS)}")
    if p["enabled"]:
        _check_repeats(cfg)
    return cfg


def _check_repeats(cfg):
    """c2c и mem — системный сбор PEBS на тех же ядрах, и он вытесняет per-task
    события: замерено, что при c2c в том же повторе perf record падает с 3000
    сэмплов до 6, а perf stat уезжает в multiplexing на 30%. Поэтому наборы
    повторов обязаны не пересекаться."""
    p, total = cfg["profile"], cfg["bench"]["repeats"]
    sampling = any(t != "stat" for role in ROLES for t in p[role]["tools"])
    role_reps = set(p["repeats"] or range(1, total + 1))
    shared_reps = set(p["shared_repeats"] or range(1, total + 1)) if p["shared"] else set()
    if sampling and (both := role_reps & shared_reps):
        print(f"warning: повторы {sorted(both)} просят и профайлеры по ролям, и "
              f"{p['shared']} — системный сбор вытеснит per-task события, профили "
              f"выйдут пустыми. Разведите profile.repeats и profile.shared_repeats.")
    for name, reps in (("repeats", p["repeats"]), ("shared_repeats", p["shared_repeats"])):
        if out := [r for r in reps if r > total]:
            print(f"warning: profile.{name} просит повторы {out}, а "
                  f"bench.repeats = {total} — они не наступят")


def active_role(cfg, rep):
    """Профилируем ли этот повтор по ролям. repeats = [] означает все."""
    p = cfg["profile"]
    return p["enabled"] and (not p["repeats"] or rep in p["repeats"])


def active_shared(cfg, rep):
    p = cfg["profile"]
    return (p["enabled"] and bool(p["shared"])
            and (not p["shared_repeats"] or rep in p["shared_repeats"]))


def tools_used(cfg, rep):
    """Строка для колонки profiled в summary.csv: что реально висело на повторе."""
    p, used = cfg["profile"], set()
    if active_role(cfg, rep):
        used |= {t for role in ROLES for t in p[role]["tools"]}
    if active_shared(cfg, rep):
        used |= set(p["shared"])
    return "+".join(sorted(used)) if used else "-"


def pmu_for(cpu):
    """cpu_core / cpu_atom / None (ядро не задано или PMU не гибридный).

    На гибридном чипе обобщённые имена событий разворачиваются в оба PMU, и
    половина строк выходит `<not counted>`; perf mem с обоими PMU в группе просто
    падает по ENOENT. Поэтому PMU выбирается явно по ядру, на которое запинена роль.
    """
    if cpu is None:
        return None
    for pmu in ("cpu_core", "cpu_atom"):
        mask = Path(f"/sys/devices/{pmu}/cpus")
        if mask.exists() and int(cpu) in expand(mask.read_text().strip()):
            return pmu
    return None


def _ev(name, pmu):
    return f"{pmu}/{name}/" if pmu else name


class Profiler:
    """Один процесс-сборщик и то, как из его сырья сделать читаемый артефакт."""

    def __init__(self, kind, base, argv, pid_scoped, out, pmu=None, title=""):
        self.kind, self.base, self.argv = kind, base, argv
        self.pid_scoped, self.out, self.pmu, self.title = pid_scoped, out, pmu, title
        self.proc = None
        # От out, а не от kind: record и cache-miss делят kind, но не файлы.
        self.err = out.with_suffix(".err")

    def start(self, collector_cpu):
        self.base.parent.mkdir(parents=True, exist_ok=True)
        argv = ["taskset", "-c", str(collector_cpu)] + self.argv
        with self.err.open("w") as e:
            self.proc = subprocess.Popen(argv, stdout=e, stderr=subprocess.STDOUT)
        return self


def _perf(kind, base, suffix, args, pid_scoped, pmu=None, title=""):
    out = base.with_name(base.name + suffix)
    return Profiler(kind, base, ["perf"] + args + ["-o", str(out)], pid_scoped, out,
                    pmu, title)


def attach_role(role, pid, cpu, base, cfg):
    """Сборщики для одного бинарника. base — путь артефактов без расширения."""
    p = cfg["profile"]
    pmu, out = pmu_for(cpu), []
    for tool in p[role]["tools"]:
        if tool == "stat":
            events = ",".join([_ev(e, pmu) for e in STAT_HW] + STAT_SW)
            out.append(Profiler("stat", base, [
                "perf", "stat", "-e", events,
                "-o", str(base.with_name(base.name + ".stat.txt")), "-p", str(pid)],
                True, base.with_name(base.name + ".stat.txt")))
            # topdown отдельным вызовом: у него своя фиксированная группа событий,
            # в один -e с обычным списком она не лезет. Счётчики, стоят копейки.
            td = ["perf", "stat", "--topdown"]
            if pmu:
                td += ["--cputype", pmu.removeprefix("cpu_")]
            out.append(Profiler("topdown", base, td + [
                "-o", str(base.with_name(base.name + ".topdown.txt")), "-p", str(pid)],
                True, base.with_name(base.name + ".topdown.txt")))
        elif tool == "record":
            out.append(_perf("record", base, ".perf.data", [
                "record", "-q", "--no-buildid-cache", "-F", str(p["freq"]),
                "-g", "--call-graph", "fp", "-p", str(pid)], True,
                title=f"{base.name} cycles"))
        elif tool == "cache-miss":
            # Частота, а не фиксированный период: у этой нагрузки рабочее множество
            # почти целиком в L1, промахов единицы тысяч в секунду. -c 2000 давал
            # 12 сэмплов на прогон, -F сам опускает период до потока событий.
            out.append(_perf("record", base, ".cachemiss.data", [
                "record", "-q", "--no-buildid-cache", "-F", str(p["freq"]),
                "-e", _ev("cache-misses", pmu) + ("pp" if pmu else ":pp"),
                "-g", "--call-graph", "fp", "-p", str(pid)], True,
                title=f"{base.name} cache-misses"))
    return [pr.start(p["collector_cpu"]) for pr in out]


def attach_shared(cpus, base, cfg):
    """Сборщики на точку целиком: c2c и mem видят обе стороны строки кэша."""
    p = cfg["profile"]
    cpulist = ",".join(str(c) for c in sorted(set(cpus)))
    out = []
    for tool in p["shared"]:
        if tool == "c2c":
            out.append(_perf("c2c", base, ".c2c.data",
                             ["c2c", "record", "-C", cpulist], False))
        elif tool == "mem":
            out.append(_perf("mem", base, ".mem.data",
                             ["mem", "record", "-C", cpulist], False))
    return [pr.start(p["collector_cpu"]) for pr in out]


def stop_all(profs):
    """Вызывается из Chain.__exit__ в bench.py, уже после смерти целей: pid-scoped
    сборщики к этому моменту досыпают сами, cpu-scoped глушим SIGTERM'ом — perf
    record по нему штатно дописывает perf.data."""
    for pr in profs:
        if not pr.pid_scoped and pr.proc.poll() is None:
            pr.proc.terminate()
    for pr in profs:
        try:
            pr.proc.wait(30)
        except subprocess.TimeoutExpired:
            print(f"  профайлер {pr.kind} не завершился за 30 с, убиваю")
            pr.proc.kill()
            pr.proc.wait()          # без этого остался бы зомби и висящий сбор
        if pr.err.exists() and not pr.err.stat().st_size:
            pr.err.unlink()


def _pipe(stages, out):
    """stages -> файл. Ошибка любого звена не должна ронять прогон."""
    try:
        with out.open("w") as f:
            prev = None
            procs = []
            for i, argv in enumerate(stages):
                last = i == len(stages) - 1
                pr = subprocess.Popen(argv, stdin=prev,
                                      stdout=f if last else subprocess.PIPE,
                                      stderr=subprocess.DEVNULL)
                if prev:
                    prev.close()
                prev = pr.stdout
                procs.append(pr)
            for pr in procs:
                pr.wait()
        return out.stat().st_size > 0
    except OSError as e:
        print(f"  {out.name}: {e}")
        return False


def _flamegraph(pr, warned):
    """perf script -> свёртка -> SVG вендоренными скриптами Brendan Gregg."""
    folded = pr.out.with_suffix(".folded")
    collapse, flame = VENDOR / "stackcollapse-perf.pl", VENDOR / "flamegraph.pl"
    script = ["perf", "script", "-i", str(pr.out), "--no-inline",
              "-F", "comm,pid,tid,ip,sym,dso"]
    if not collapse.exists():
        # .folded всё равно не получить без stackcollapse, но сырьё для него сохраняем.
        _pipe([script], pr.out.with_suffix(".script.txt"))
        if not warned:
            print(f"\nнет {collapse.name}/{flame.name} — SVG пропущены. Положите их в "
                  f"{VENDOR}:\n"
                  f"  mkdir -p {VENDOR} && cd {VENDOR} && curl -LO "
                  f"https://raw.githubusercontent.com/brendangregg/FlameGraph/master/"
                  f"stackcollapse-perf.pl -LO https://raw.githubusercontent.com/"
                  f"brendangregg/FlameGraph/master/flamegraph.pl && chmod +x *.pl\n")
        return True
    if not _pipe([script, ["perl", str(collapse)]], folded):
        # Пустая свёртка = либо в perf.data нет стеков, либо сам collapse битый
        # (скачанный по curl HTML-ответ вместо скрипта — проверяем это явно).
        if subprocess.run(["perl", "-c", str(collapse)],
                          capture_output=True).returncode:
            print(f"  {collapse.name} не является рабочим perl-скриптом — "
                  f"перекачайте его; SVG пропущены")
        else:
            print(f"  {folded.name}: пустая свёртка, SVG пропущен")
        return warned
    if flame.exists():
        _pipe([["perl", str(flame), "--title", pr.title, "--colors", "java",
                str(folded)]], pr.out.with_suffix(".flame.svg"))
    return warned


def postprocess_all(profs):
    """Разбор сырья: perf report грузит ядро на десятки секунд, поэтому вызывается
    один раз после всего свипа, а не между точками."""
    warned = False
    for pr in profs:
        if not pr.out.exists():
            continue
        if pr.kind == "record":
            _pipe([["perf", "report", "-i", str(pr.out), "--stdio", "--no-children",
                    "-g", "none", "--percent-limit", "0.5"]],
                  pr.out.with_suffix(".report.txt"))
            warned = _flamegraph(pr, warned)
        elif pr.kind == "c2c":
            _pipe([["perf", "c2c", "report", "-i", str(pr.out), "--stdio"]],
                  pr.out.with_suffix(".txt"))
        elif pr.kind == "mem":
            _pipe([["perf", "mem", "report", "-i", str(pr.out), "--stdio"]],
                  pr.out.with_suffix(".txt"))
