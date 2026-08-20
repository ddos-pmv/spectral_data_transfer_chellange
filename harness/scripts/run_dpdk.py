#!/usr/bin/env python3
"""Однохостовый стенд фазы 0: вся цепочка поверх veth-пары и AF_XDP.

    producer --> /fanout_ring (shm)
             --> dpdk_sender  [veth-tx] ==AF_XDP/UDP==> [veth-rx] dpdk_reciever
             --> /fanout_ring_rx (shm)
             --> consumer --> перцентили + CSV

    sudo ./scripts/run_dpdk.py

Нужен root: huge-страницы лежат в /dev/hugepages (владелец root), а AF_XDP
грузит в ядро XDP-программу (CAP_NET_ADMIN + CAP_BPF).

Флагов у скрипта намеренно нет — всё, что настраивается, лежит константами
ниже. Это стенд для отладки, а не для измерений: sender и receiver делят
одну машину, одну память и один LLC, так что абсолютные числа отсюда в
решение не идут. Настоящие замеры — фаза 1, две машины через кабель.
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

HARNESS = Path(__file__).resolve().parent.parent

# Где искать бинари: первый существующий каталог из списка.
BUILD_DIRS = ["cmake-build-release", "build", "bin"]

VETH_TX, VETH_RX = "veth-tx", "veth-rx"

# Ядра. Бери P-ядра и через одно, чтобы два busy-spin потока не сели на
# HT-пару одного физического ядра (см. `lscpu -e`, колонка CORE).
# 12–15 на i7-12650H — E-ядра, для замеров латентности не годятся.
CPU_SENDER, CPU_RECEIVER = 2, 6
CPU_PRODUCER, CPU_CONSUMER = 8, 10

SHM_TX = "/fanout_ring"
SHM_RX = "/fanout_ring_rx"
SLOTS = 1024

RATE = 500_000        # msg/s у продюсера; 0 = без троттлинга
COUNT = 1_000_000       # сколько замеров пишет consumer, потом выходит сам
WARMUP = 20_000       # съедается и выбрасывается до начала записи
MSG_TYPE = "mixed"

BURST = 32
UDP_PORT = 40001
SRC_IP, DST_IP = "10.0.0.1", "10.0.0.2"

HUGEPAGES_2M = 2048   # 2048 * 2 МБ = 4 ГБ


# ---------------------------------------------------------------- утилиты

def sh(*argv, check=True, quiet=False):
    """Синхронная команда. quiet=True — не падать и не шуметь."""
    r = subprocess.run([str(a) for a in argv],
                       capture_output=True, text=True)
    if r.returncode != 0 and check and not quiet:
        sys.exit(f"FAILED: {' '.join(str(a) for a in argv)}\n{r.stderr}")
    return r.stdout


def need_root():
    if os.geteuid() != 0:
        sys.exit("нужен root: sudo ./scripts/run_dpdk.py")


def find_build_dir():
    for name in BUILD_DIRS:
        d = HARNESS / name
        if (d / "dpdk_sender").exists():
            return d
    sys.exit(f"не нашёл dpdk_sender ни в одном из {BUILD_DIRS} — собери проект")


# ------------------------------------------------------------- окружение

def ensure_hugepages():
    """2 МБ страницы резервируются на живой системе, 1 ГБ — только через GRUB."""
    node = Path("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages")
    have = int(node.read_text())
    if have < HUGEPAGES_2M:
        node.write_text(str(HUGEPAGES_2M))
        have = int(node.read_text())
    free = int(sh("grep", "-oP", r"HugePages_Free:\s+\K\d+", "/proc/meminfo").strip()
               or 0)
    print(f"hugepages: total={have} free={free}")
    if free == 0:
        sys.exit("свободных huge-страниц нет — остались процессы с прошлого "
                 "запуска? проверь /dev/hugepages")


def setup_veth():
    """Пересоздаём пару каждый раз: так offload'ы гарантированно сняты, а
    MAC'и читаются свежие (при пересоздании они генерируются заново)."""
    sh("ip", "link", "del", VETH_TX, check=False, quiet=True)
    sh("ip", "link", "add", VETH_TX, "type", "veth", "peer", "name", VETH_RX)

    for i in (VETH_TX, VETH_RX):
        sh("ip", "link", "set", i, "up")
        # Фальшивые контрольные суммы и GRO/GSO мешают сырому AF_XDP-пути.
        sh("ethtool", "-K", i, "tx", "off", "rx", "off",
           "gro", "off", "gso", "off", "tso", "off", check=False, quiet=True)
        # Одна очередь: XDP-сокет привязан к очереди 0, а RSS раскидал бы
        # пакеты по остальным, и мы бы их просто не увидели.
        sh("ethtool", "-L", i, "combined", "1", check=False, quiet=True)

    macs = {i: json.loads(sh("ip", "-json", "link", "show", i))[0]["address"]
            for i in (VETH_TX, VETH_RX)}
    print(f"veth: {VETH_TX}={macs[VETH_TX]}  {VETH_RX}={macs[VETH_RX]}")
    return macs[VETH_TX], macs[VETH_RX]


def clean_state():
    """Хвосты от упавших прогонов: shm-сегменты, файлы huge-страниц и
    runtime-каталоги EAL. Без этой уборки следующий запуск подхватывает
    чужое состояние и ведёт себя загадочно.

    Вызывается и до, и после прогона. После — обязательно: всё, что мы
    создали, принадлежит root'у, и обычный bench.py потом не сможет
    даже удалить /dev/shm/fanout_ring (Operation not permitted)."""
    for name in ("fanout_ring", "fanout_ring_rx"):
        Path(f"/dev/shm/{name}").unlink(missing_ok=True)
    for prefix in ("tx", "rx"):
        shutil.rmtree(f"/var/run/dpdk/{prefix}", ignore_errors=True)
        for f in Path("/dev/hugepages").glob(f"{prefix}map_*"):
            f.unlink(missing_ok=True)


def give_back(path):
    """Возвращаем логи и CSV тому, кто звал sudo. Иначе results/ обрастает
    root-каталогами, и всё остальное (bench.py, ноутбук, IDE) об них спотыкается."""
    uid, gid = os.environ.get("SUDO_UID"), os.environ.get("SUDO_GID")
    if uid is None or gid is None:
        return
    uid, gid = int(uid), int(gid)
    os.chown(path, uid, gid)
    for p in Path(path).rglob("*"):
        os.chown(p, uid, gid)


# --------------------------------------------------------------- запуск

class Chain:
    """Процессы цепочки + их логи. Гарантирует, что всё будет прибито."""

    def __init__(self, rundir):
        self.rundir = rundir
        self.procs = []          # [(имя, Popen)]

    def spawn(self, name, cpu, argv):
        log = self.rundir / f"{name}.log"
        # taskset не даёт планировщику мигрировать busy-spin поток между
        # ядрами; setarch -R выключает ASLR, чтобы раскладка памяти не
        # плавала от прогона к прогону (так же делает bench.py).
        pre = ["taskset", "-c", str(cpu), "setarch", "-R"]
        with log.open("w") as f:
            p = subprocess.Popen(pre + [str(a) for a in argv],
                                 stdout=f, stderr=subprocess.STDOUT)
        self.procs.append((name, p))
        print(f"  {name:<14} cpu={cpu} pid={p.pid}")
        return p

    def wait_shm(self, path, proc, timeout=10.0):
        """Ждём, пока процесс реально создаст свой сегмент. Без этого
        следующий в цепочке стартует раньше и падает на open()."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if Path(path).exists():
                return
            if proc.poll() is not None:
                sys.exit(f"процесс умер, не создав {path} — смотри логи в {self.rundir}")
            time.sleep(0.01)
        sys.exit(f"не дождались {path}")

    def shutdown(self):
        """Гасим в обратном порядке: сначала транспорт, потом источник.
        SIGTERM -> дать досчитать и напечатать статистику -> SIGKILL."""
        for name, p in reversed(self.procs):
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
        for name, p in reversed(self.procs):
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                print(f"  {name} не отреагировал на SIGTERM, убиваю")
                p.kill()


def eal(cpu, prefix, iface):
    """Аргументы EAL. -l <cpu> даёт один lcore — он же main lcore, на нём
    всё и работает: worker'ы нам не нужны, одна очередь = одно ядро."""
    return ["-l", str(cpu),
            "--file-prefix", prefix,
            "--vdev", f"net_af_xdp0,iface={iface},start_queue=0,queue_count=1",
            "--"]


def main():
    need_root()
    build = find_build_dir()
    print(f"бинари: {build}")

    # Уборка до проверки свободных страниц, а не после: иначе счётчик
    # HugePages_Free показывает занятое мусором с прошлого прогона.
    clean_state()
    ensure_hugepages()
    mac_tx, mac_rx = setup_veth()

    rundir = HARNESS / "results" / datetime.now().strftime("dpdk-%Y%m%d-%H%M%S")
    rundir.mkdir(parents=True)
    csv = rundir / "latencies.csv"
    print(f"логи: {rundir}\n")

    chain = Chain(rundir)
    try:
        # 1. Источник. Создаёт /fanout_ring, работает без ограничения по
        #    количеству — остановим сигналом, когда consumer наберёт выборку.
        p = chain.spawn("producer", CPU_PRODUCER, [
            build / "producer",
            "--shm", SHM_TX, "--slots", SLOTS,
            "--count", 0, "--rate", RATE, "--type", MSG_TYPE,
        ])
        chain.wait_shm(f"/dev/shm{SHM_TX}", p)

        # 2. Приёмник. Создаёт /fanout_ring_rx, поэтому идёт до consumer'а.
        r = chain.spawn("dpdk_reciever", CPU_RECEIVER,
                        [build / "dpdk_reciever", *eal(CPU_RECEIVER, "rx", VETH_RX),
                         "--shm", SHM_RX, "--slots", SLOTS,
                         "--port", UDP_PORT, "--burst", BURST])
        chain.wait_shm(f"/dev/shm{SHM_RX}", r)

        # 3. Отправитель. dst-mac — это MAC того конца veth, куда мы шлём:
        #    ARP-стека у нас нет, спросить "чей это IP" некому.
        chain.spawn("dpdk_sender", CPU_SENDER,
                    [build / "dpdk_sender", *eal(CPU_SENDER, "tx", VETH_TX),
                     "--shm", SHM_TX, "--slots", SLOTS,
                     "--dst-mac", mac_rx,
                     "--src-ip", SRC_IP, "--dst-ip", DST_IP,
                     "--dst-port", UDP_PORT, "--burst", BURST])

        # 4. Точка измерения. --from-edge: встаём на живой край и не считаем
        #    пропущенный до подключения префикс за потери. Выходит сам, набрав
        #    --count замеров.
        c = chain.spawn("consumer", CPU_CONSUMER, [
            build / "consumer",
            "--shm", SHM_RX, "--slots", SLOTS, "--from-edge",
            "--count", COUNT, "--warmup", WARMUP, "--csv", csv,
        ])

        print(f"\nждём {COUNT} замеров...")
        c.wait()
    finally:
        chain.shutdown()
        clean_state()
        give_back(rundir)

    # Статистику печатают все четверо, и сверять их надо вместе: расхождение
    # sender.sent и receiver.received — это потери на канале, а lapped и
    # tx_dropped — наши собственные, к сети отношения не имеющие.
    for name in ("dpdk_sender", "dpdk_reciever", "consumer"):
        log = rundir / f"{name}.log"
        if log.exists():
            print(f"\n=========== {name} ===========")
            print(log.read_text().strip())

    print(f"\nCSV для ноутбука: {csv}")


if __name__ == "__main__":
    main()
