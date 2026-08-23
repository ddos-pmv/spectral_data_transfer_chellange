#!/usr/bin/env bash
# Принимающая сторона: сеть -> dpdk_reciever -> shm -> consumer.
#
# Запускать ПЕРВЫМ, до tx.sh на отправляющей машине.
# Завершается сам, когда consumer наберёт COUNT замеров.
#
#   sudo ./scripts/rx.sh
#   sudo COUNT=3000000 ./scripts/rx.sh
set -euo pipefail

IFACE=${IFACE:-ens5}                    # интерфейс, на который приходит трафик
# Устройство для EAL. По умолчанию AF_XDP поверх живого интерфейса (ядро видит
# карту, SSH не теряется). Для ENA PMD передайте адрес PCI, привязанный к
# igb_uio:  EAL_DEV="-a 0000:28:00.0"  — см. AWS.md, раздел 8.
EAL_DEV=${EAL_DEV:-"--vdev net_af_xdp0,iface=$IFACE,start_queue=0,queue_count=1"}
ENI_INDEX=${ENI_INDEX:-1}               # device-index ENI, отданной DPDK (режим А)
PORT=${PORT:-40001}
COUNT=${COUNT:-1000000}                 # замеров, после них consumer выходит
WARMUP=${WARMUP:-20000}                 # съедается и выбрасывается до записи
IDLE_MS=${IDLE_MS:-2000}                # тишина дольше этого = отправитель кончился
SLOTS=${SLOTS:-1024}
BURST=${BURST:-32}
CPU_RECEIVER=${CPU_RECEIVER:-0}
CPU_CONSUMER=${CPU_CONSUMER:-1}

cd "$(dirname "$0")/.."
. scripts/dpdk-eni.sh
[ "$(id -u)" = 0 ] || { echo "нужен root: hugepages и AF_XDP требуют привилегий"; exit 1; }
[ -x bin/dpdk_reciever ] || { echo "нет bin/dpdk_reciever — соберите: make -j\$(nproc)"; exit 1; }

# Печатаем адреса той карты, которую слушает DPDK. В режиме ядра это $IFACE,
# в режиме PMD карта ядру не видна — берём из метаданных инстанса.
echo "приёмная машина: подставить в tx.sh --"
if kernel_mode; then
  ip -o link show "$IFACE" | awk '{print "    PEER_MAC="$(NF-2)}'
  ip -4 -o addr show "$IFACE" | awk '{split($4,a,"/"); print "    DST_IP="a[1]}'
else
  ADDRS=$(eni_addrs "$ENI_INDEX") || ADDRS=""
  if [ -n "$ADDRS" ]; then
    read -r ENI_MAC ENI_IP <<<"$ADDRS"
    echo "    PEER_MAC=$ENI_MAC"
    echo "    DST_IP=$ENI_IP"
  else
    echo "    (не удалось прочитать ENI $ENI_INDEX из метаданных — задайте вручную)"
  fi
fi

RUN=results/rx-$(date +%Y%m%d-%H%M%S)
mkdir -p "$RUN"
rm -f /dev/shm/fanout_ring_rx

RECEIVER=
cleanup() {
  # SIGTERM, а не kill: ресивер по нему допечатывает свою статистику.
  [ -n "$RECEIVER" ] && kill -TERM "$RECEIVER" 2>/dev/null || true
  [ -n "$RECEIVER" ] && wait "$RECEIVER" 2>/dev/null || true
  rm -f /dev/shm/fanout_ring_rx
  [ -n "${SUDO_UID:-}" ] && chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$RUN" || true
  echo
  echo "статистика приёмника: $RUN/receiver.log"
  tail -n 8 "$RUN/receiver.log"
}
trap cleanup EXIT

echo "rx: слушаю udp/$PORT, жду $COUNT сообщений ($EAL_DEV)"
echo "    логи: $RUN"

taskset -c "$CPU_RECEIVER" setarch -R ./bin/dpdk_reciever \
  -l "$CPU_RECEIVER" --file-prefix rx \
  $EAL_DEV \
  -- --shm /fanout_ring_rx --slots "$SLOTS" --port "$PORT" --burst "$BURST" \
  > "$RUN/receiver.log" 2>&1 &
RECEIVER=$!

# Кольцо создаёт ресивер; consumer открывает уже готовое.
for _ in $(seq 50); do [ -e /dev/shm/fanout_ring_rx ] && break; sleep 0.1; done
[ -e /dev/shm/fanout_ring_rx ] || { echo "ресивер не поднялся, см. $RUN/receiver.log"; exit 1; }

taskset -c "$CPU_CONSUMER" setarch -R ./bin/consumer \
  --shm /fanout_ring_rx --slots "$SLOTS" --from-edge \
  --count "$COUNT" --warmup "$WARMUP" --idle-ms "$IDLE_MS" \
  --csv "$RUN/latencies.csv" \
  2>&1 | tee "$RUN/consumer.log"
