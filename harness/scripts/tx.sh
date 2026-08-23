#!/usr/bin/env bash
# Отправляющая сторона: producer -> shm -> dpdk_sender -> сеть.
#
# Запускать ВТОРЫМ, после rx.sh на приёмной машине.
# Останавливать Ctrl+C: оба процесса допечатают статистику.
#
#   sudo PEER_MAC=06:aa:.. DST_IP=172.31.5.20 ./scripts/tx.sh
#   sudo PEER_MAC=.. DST_IP=.. RATE=500000 BURST=8 ./scripts/tx.sh
set -euo pipefail

IFACE=${IFACE:-ens5}                    # интерфейс, через который уходит трафик
# Устройство для EAL. По умолчанию AF_XDP поверх живого интерфейса (ядро видит
# карту, SSH не теряется). Для ENA PMD передайте адрес PCI, привязанный к
# vfio-pci:  EAL_DEV="-a 0000:00:06.0,llq_policy=3"  — см. AWS.md.
EAL_DEV=${EAL_DEV:-"--vdev net_af_xdp0,iface=$IFACE,start_queue=0,queue_count=1"}
PEER_MAC=${PEER_MAC:?укажите MAC приёмной машины (ip link show <iface> на ней)}
DST_IP=${DST_IP:?укажите приватный IP приёмной машины}
SRC_IP=${SRC_IP:-}                      # пусто = взять с IFACE
DST_PORT=${DST_PORT:-40001}
RATE=${RATE:-200000}                    # msg/s, 0 = без троттлинга
TYPE=${TYPE:-mixed}                     # trade | bbo | book | mixed
SLOTS=${SLOTS:-1024}
BURST=${BURST:-32}
CPU_PRODUCER=${CPU_PRODUCER:-0}
CPU_SENDER=${CPU_SENDER:-1}

cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "нужен root: hugepages и AF_XDP требуют привилегий"; exit 1; }
[ -x bin/dpdk_sender ] || { echo "нет bin/dpdk_sender — соберите: make -j\$(nproc)"; exit 1; }

# VPC режет пакеты, у которых src IP не совпадает с адресом ENI.
[ -n "$SRC_IP" ] || SRC_IP=$(ip -4 -o addr show "$IFACE" | awk '{print $4}' | cut -d/ -f1)
[ -n "$SRC_IP" ] || { echo "у $IFACE нет IPv4-адреса"; exit 1; }

RUN=results/tx-$(date +%Y%m%d-%H%M%S)
mkdir -p "$RUN"
rm -f /dev/shm/fanout_ring

PRODUCER=
cleanup() {
  [ -n "$PRODUCER" ] && kill -TERM "$PRODUCER" 2>/dev/null || true
  [ -n "$PRODUCER" ] && wait "$PRODUCER" 2>/dev/null || true
  rm -f /dev/shm/fanout_ring
  [ -n "${SUDO_UID:-}" ] && chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$RUN" || true
}
trap cleanup EXIT

echo "tx: $SRC_IP -> $DST_IP:$DST_PORT ($PEER_MAC) через $IFACE, rate=$RATE"
echo "    логи: $RUN"

taskset -c "$CPU_PRODUCER" setarch -R ./bin/producer \
  --shm /fanout_ring --slots "$SLOTS" --count 0 --rate "$RATE" --type "$TYPE" \
  > "$RUN/producer.log" 2>&1 &
PRODUCER=$!

# Кольцо должно существовать до старта отправителя: он открывает его с create=false.
for _ in $(seq 50); do [ -e /dev/shm/fanout_ring ] && break; sleep 0.1; done
[ -e /dev/shm/fanout_ring ] || { echo "producer не создал кольцо, см. $RUN/producer.log"; exit 1; }

taskset -c "$CPU_SENDER" setarch -R ./bin/dpdk_sender \
  -l "$CPU_SENDER" --file-prefix tx \
  $EAL_DEV \
  -- --shm /fanout_ring --slots "$SLOTS" \
     --dst-mac "$PEER_MAC" --src-ip "$SRC_IP" --dst-ip "$DST_IP" \
     --dst-port "$DST_PORT" --burst "$BURST" 2>&1 | tee "$RUN/sender.log"
