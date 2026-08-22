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
PORT=${PORT:-40001}
COUNT=${COUNT:-1000000}                 # замеров, после них consumer выходит
WARMUP=${WARMUP:-20000}                 # съедается и выбрасывается до записи
SLOTS=${SLOTS:-1024}
BURST=${BURST:-32}
CPU_RECEIVER=${CPU_RECEIVER:-0}
CPU_CONSUMER=${CPU_CONSUMER:-1}

cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "нужен root: hugepages и AF_XDP требуют привилегий"; exit 1; }
[ -x bin/dpdk_reciever ] || { echo "нет bin/dpdk_reciever — соберите: make -j\$(nproc)"; exit 1; }

echo "приёмная машина: MAC и IP для tx.sh --"
ip -o link show "$IFACE" | awk '{print "    PEER_MAC="$(NF-2)}'
ip -4 -o addr show "$IFACE" | awk '{split($4,a,"/"); print "    DST_IP="a[1]}'

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

echo "rx: слушаю udp/$PORT на $IFACE, жду $COUNT сообщений"
echo "    логи: $RUN"

taskset -c "$CPU_RECEIVER" setarch -R ./bin/dpdk_reciever \
  -l "$CPU_RECEIVER" --file-prefix rx \
  --vdev "net_af_xdp0,iface=$IFACE,start_queue=0,queue_count=1" \
  -- --shm /fanout_ring_rx --slots "$SLOTS" --port "$PORT" --burst "$BURST" \
  > "$RUN/receiver.log" 2>&1 &
RECEIVER=$!

# Кольцо создаёт ресивер; consumer открывает уже готовое.
for _ in $(seq 50); do [ -e /dev/shm/fanout_ring_rx ] && break; sleep 0.1; done
[ -e /dev/shm/fanout_ring_rx ] || { echo "ресивер не поднялся, см. $RUN/receiver.log"; exit 1; }

taskset -c "$CPU_CONSUMER" setarch -R ./bin/consumer \
  --shm /fanout_ring_rx --slots "$SLOTS" --from-edge \
  --count "$COUNT" --warmup "$WARMUP" --csv "$RUN/latencies.csv" \
  2>&1 | tee "$RUN/consumer.log"
