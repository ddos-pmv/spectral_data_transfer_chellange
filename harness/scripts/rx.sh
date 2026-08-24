#!/usr/bin/env bash
# Приёмная сторона: сеть -> dpdk_reciever -> shm -> consumer.
#
# Запускать ПЕРВЫМ, до tx.sh на отправляющей машине. Завершается сам, когда
# consumer наберёт COUNT замеров или замолчит отправитель (IDLE_MS).
#
#   sudo ./scripts/rx.sh
#   sudo COUNT=3000000 IDLE_MS=60000 ./scripts/rx.sh
#
# Карта, ядра и порт берутся из scripts/host.conf.
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/lib.sh"
cd "$ROOT_DIR"

COUNT=${COUNT:-1000000}       # замеров, после них consumer выходит
WARMUP=${WARMUP:-20000}       # первые замеры делаются и выбрасываются
IDLE_MS=${IDLE_MS:-2000}      # тишина дольше этого = отправитель кончился
SLOTS=${SLOTS:-1024}
BURST=${BURST:-32}

need_root
[ -x bin/dpdk_reciever ] || die "нет bin/dpdk_reciever -- make -j\$(nproc)"
DEV=$(eal_dev)

# Печатаем адреса той карты, которую слушает DPDK: в режиме PMD ядру она не
# видна, и MAC с IP взять больше неоткуда, кроме метаданных инстанса.
say "Вписать в host.conf отправляющей машины"
if kernel_mode; then
  row PEER_MAC "$(cat "/sys/class/net/$DPDK_IFACE/address")"
  row PEER_IP "$(local_ip)"
elif ADDRS=$(eni_addrs "$DPDK_ENI_INDEX"); then
  row PEER_MAC "$(awk '{print $1}' <<<"$ADDRS")"
  row PEER_IP "$(awk '{print $2}' <<<"$ADDRS")"
else
  bad "IMDS не ответил -- задайте адреса вручную"
fi

RUN=results/rx-$(date +%Y%m%d-%H%M%S)
mkdir -p "$RUN"
rm -f /dev/shm/fanout_ring_rx

RECEIVER=
cleanup() {
  # SIGTERM, а не kill: по нему ресивер допечатывает статистику.
  [ -n "$RECEIVER" ] && kill -TERM "$RECEIVER" 2>/dev/null || true
  [ -n "$RECEIVER" ] && wait "$RECEIVER" 2>/dev/null || true
  rm -f /dev/shm/fanout_ring_rx
  [ -n "${SUDO_UID:-}" ] && chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$RUN" || true
  echo
  echo "статистика приёмника: $RUN/receiver.log"
  tail -n 8 "$RUN/receiver.log"
}
trap cleanup EXIT

say "Приём"
row порт "udp/$PORT"
row устройство "$DEV"
row ядра "приёмник $CPU_RECEIVER, consumer $CPU_CONSUMER"
row ждём "$COUNT сообщений"
row логи "$RUN"

taskset -c "$CPU_RECEIVER" setarch -R ./bin/dpdk_reciever \
  -l "$CPU_RECEIVER" --file-prefix rx \
  $DEV \
  -- --shm /fanout_ring_rx --slots "$SLOTS" --port "$PORT" --burst "$BURST" \
  > "$RUN/receiver.log" 2>&1 &
RECEIVER=$!

# Кольцо создаёт ресивер; consumer открывает уже готовое.
for _ in $(seq 50); do [ -e /dev/shm/fanout_ring_rx ] && break; sleep 0.1; done
[ -e /dev/shm/fanout_ring_rx ] || die "ресивер не поднялся, см. $RUN/receiver.log"

taskset -c "$CPU_CONSUMER" setarch -R ./bin/consumer \
  --shm /fanout_ring_rx --slots "$SLOTS" --from-edge \
  --count "$COUNT" --warmup "$WARMUP" --idle-ms "$IDLE_MS" \
  --csv "$RUN/latencies.csv" \
  2>&1 | tee "$RUN/consumer.log"
