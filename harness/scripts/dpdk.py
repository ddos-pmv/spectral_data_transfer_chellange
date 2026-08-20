#!/usr/bin/env python3
"""Стартер DPDK-цепочки по TOML-конфигу.

    sudo ./scripts/dpdk.py [config.toml]     # по умолчанию scripts/dpdk.toml

Скрипт не знает, что такое producer или sender: он запускает процессы из
[[proc]] по порядку, рендеря args/eal в флаги, ждёт завершения последнего и
гасит остальных. Логи — в results/<ts>/r<N>-<name>.log.

Стенд (veth-пара, huge-страницы, isolcpus) поднимается ОДИН РАЗ руками --
команды в шапке dpdk.toml. Скрипт его только проверяет и печатает, что видит:
состояние стенда должно быть воспроизводимым и зафиксированным в описании
решения, а не пересоздаваться неявно на каждом прогоне.

Нужен root: huge-страницы в /dev/hugepages и загрузка XDP-программы.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import tomllib
from datetime import datetime
from pathlib import Path

HARNESS = Path(__file__).resolve().parent.parent
BIN = HARNESS / "bin"
PCTS = ["p50", "p99", "p99.9", "p99.99"]


def as_user(argv, cwd=None):
    """Запуск от имени того, кто звал sudo. Собирать под root нельзя: объектники
    в bin/ станут root'овыми, и IDE потом не сможет пересобрать проект сама."""
    kw = {}
    uid, gid = os.environ.get("SUDO_UID"), os.environ.get("SUDO_GID")
    if uid:
        home = os.path.expanduser("~" + os.environ.get("SUDO_USER", ""))
        kw = dict(user=int(uid), group=int(gid), env={**os.environ, "HOME": home})
    r = subprocess.run([str(a) for a in argv], capture_output=True, text=True,
                       cwd=cwd, **kw)
    if r.returncode != 0:
        sys.exit(f"FAILED: {' '.join(str(a) for a in argv)}\n"
                 f"{r.stdout[-4000:]}\n{r.stderr[-4000:]}")
    return r.stdout


def build():
    """Пересборка каждый раз: иначе меряется то, что лежало в bin/. -B, а не
    инкрементально, ровно как в bench.py."""
    as_user(["make", "-B", "-j", os.cpu_count()], cwd=HARNESS)


def flags(d):
    """{'shm': '/x', 'from-edge': True, 'l': 2} -> ['--shm','/x','--from-edge','-l','2']"""
    out = []
    for k, v in d.items():
        if v is False:
            continue
        out.append(f"-{k}" if len(k) == 1 else f"--{k}")
        if v is not True:
            out.append(str(v))
    return out


def check_env(env):
    """Только смотрим и печатаем, ничего не меняем. Напечатанный MAC -- это то,
    что должно стоять в dst-mac у сендера; расхождение здесь означает прогон
    с нулём принятых пакетов, и лучше увидеть его до, а не после."""
    for key in ("veth_tx", "veth_rx"):
        iface = env.get(key)
        if not iface:
            continue
        state = Path(f"/sys/class/net/{iface}/operstate")
        if not state.exists():
            sys.exit(f"нет интерфейса {iface} — подними стенд, см. шапку dpdk.toml")
        if state.read_text().strip() not in ("up", "unknown"):
            print(f"  ВНИМАНИЕ: {iface} не поднят  (ip link set {iface} up)")

        mac = Path(f"/sys/class/net/{iface}/address").read_text().strip()
        want = env.get(key.replace("veth", "mac"))
        note = "" if want in (None, mac) else f"   ВНИМАНИЕ: в конфиге {want}"
        print(f"  {iface:<10} {mac}{note}")

    free = int(re.search(r"HugePages_Free:\s+(\d+)",
                         Path("/proc/meminfo").read_text()).group(1))
    if free == 0:
        sys.exit("нет свободных huge-страниц:\n  sudo sh -c 'echo 2048 > "
                 "/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'")
    print(f"  hugepages  free={free}")


def clean_state():
    """Хвосты прошлых прогонов. Обязательно и ПОСЛЕ: всё созданное под root
    иначе не даст обычному bench.py даже удалить /dev/shm/fanout_ring."""
    for p in Path("/dev/shm").glob("fanout*"):
        p.unlink(missing_ok=True)
    for prefix in ("tx", "rx"):
        shutil.rmtree(f"/var/run/dpdk/{prefix}", ignore_errors=True)
        for f in Path("/dev/hugepages").glob(f"{prefix}map_*"):
            f.unlink(missing_ok=True)


def give_back(path):
    """Вернуть results/ тому, кто звал sudo, иначе IDE и ноутбук об него спотыкаются."""
    uid, gid = os.environ.get("SUDO_UID"), os.environ.get("SUDO_GID")
    if uid:
        for p in [path, *Path(path).rglob("*")]:
            os.chown(p, int(uid), int(gid))


def run_once(cfg, rundir, rep):
    """Один прогон цепочки. Возвращает лог последнего процесса (consumer)."""
    clean_state()
    procs = []
    try:
        for spec in cfg["proc"]:
            name, cpu = spec["name"], spec["cpu"]
            argv = [BIN / name, *flags(spec.get("eal", {}))]
            if "eal" in spec:
                argv.append("--")
            argv += flags(spec.get("args", {}))

            log = rundir / f"r{rep}-{name}.log"
            with log.open("w") as f:
                # taskset держит busy-spin поток на своём ядре, setarch -R
                # выключает ASLR, чтобы раскладка не плавала между прогонами.
                p = subprocess.Popen(
                    ["taskset", "-c", str(cpu), "setarch", "-R",
                     *[str(a) for a in argv]],
                    stdout=f, stderr=subprocess.STDOUT)
            procs.append(p)
            print(f"  {name:<14} cpu={cpu} pid={p.pid}")

            if "wait_shm" in spec:
                wait_shm(spec["wait_shm"], p, rundir)

        procs[-1].wait()          # последний в списке завершает прогон
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
        for p in reversed(procs):
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()

    return rundir / f"r{rep}-{cfg['proc'][-1]['name']}.log"


def wait_shm(path, proc, rundir, timeout=10.0):
    """Следующий в цепочке откроет сегмент с create=false и упадёт, если
    стартует раньше, чем предыдущий его создаст."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if Path(path).exists():
            return
        if proc.poll() is not None:
            sys.exit(f"процесс умер, не создав {path} — логи в {rundir}")
        time.sleep(0.01)
    sys.exit(f"не дождались {path} — логи в {rundir}")


def parse(log):
    """Перцентили и drop_rate из вывода consumer'а."""
    text = log.read_text()
    row = {p: re.search(rf"^\s+{re.escape(p)}\s+:\s+(\d+)", text, re.M) for p in PCTS}
    drop = re.search(r"drop_rate\s+:\s+(\S+)", text)
    return {p: (m.group(1) if m else "-") for p, m in row.items()} | \
           {"drop": drop.group(1) if drop else "-"}


def main():
    if os.geteuid() != 0:
        sys.exit("нужен root: sudo ./scripts/dpdk.py")

    path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "dpdk.toml"
    cfg = tomllib.loads(path.read_text())
    build()
    check_env(cfg.get("env", {}))

    rundir = HARNESS / "results" / datetime.now().strftime("dpdk-%Y%m%d-%H%M%S")
    rundir.mkdir(parents=True)
    shutil.copy(path, rundir / "config.toml")   # по каталогу видно, чем получено
    print(f"логи: {rundir}")

    rows = []
    try:
        for rep in range(1, cfg["bench"]["repeats"] + 1):
            print(f"\n--- прогон {rep} ---")
            rows.append(parse(run_once(cfg, rundir, rep)))
    finally:
        clean_state()
        give_back(rundir)

    print(f"\n{'rep':<5}{'drop':>10}" + "".join(f"{p:>10}" for p in PCTS))
    for i, r in enumerate(rows, 1):
        print(f"{i:<5}{r['drop']:>10}" + "".join(f"{r[p]:>10}" for p in PCTS))


if __name__ == "__main__":
    main()
