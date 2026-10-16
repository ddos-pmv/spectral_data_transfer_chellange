#!/usr/bin/env bash
# Приёмная сторона: сеть -> dpdk_reciever -> shm -> consumer.
#
# Запускать ПЕРВЫМ, до tx.sh на отправляющей машине. Завершается сам, когда
# consumer наберёт COUNT замеров или замолчит отправитель (IDLE_MS).
#
#   sudo ./scripts/rx.sh
#   sudo COUNT=3000000 IDLE_MS=60000 ./scripts/rx.sh
#   sudo REORDER=64 ./scripts/rx.sh                    # окно переупорядочивания
#   sudo RECEIVER_RXD=2048 ./scripts/rx.sh             # длиннее RX-очередь
#   sudo MTU=9001 ./scripts/rx.sh                      # jumbo, если отправитель их шлёт
#   sudo CLOCK=0 ./scripts/rx.sh                       # без замера смещения часов
#
# Размеры RX-очереди и пула mbuf -- RECEIVER_RXD/RECEIVER_MBUFS в host.conf.
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
MBUFS=${MBUFS:-8191}          # mbuf в пуле приёмника; 2^n-1 -- лучший размер для ring

# Проверяем до need_root: с опечаткой в конфиге незачем даже спрашивать пароль.
need_uint RECEIVER_RXD "$RECEIVER_RXD" 64 4096
need_pow2 RECEIVER_RXD "$RECEIVER_RXD"
need_uint RECEIVER_MBUFS "$RECEIVER_MBUFS" 1024 1048576
need_uint BURST "$BURST" 1 64
need_uint SLOTS "$SLOTS" 8 1048576
need_pow2 SLOTS "$SLOTS"
check_pool RECEIVER_MBUFS "$RECEIVER_MBUFS" RECEIVER_RXD "$RECEIVER_RXD" "$BURST"

need_root
# REORDER=N переключает на второй бинарник -- с окном переупорядочивания.
# По умолчанию 0: путь, на котором сняты все числа в RESULTS.md, не меняется.
REORDER=${REORDER:-0}
if [ "$REORDER" = 0 ]; then
  RECEIVER_BIN=bin/dpdk_reciever
  REORDER_ARGS=
else
  RECEIVER_BIN=bin/dpdk_reciever_ro
  REORDER_ARGS="--reorder $REORDER"
fi
[ -x "$RECEIVER_BIN" ] || die "нет $RECEIVER_BIN -- cmake --build build -j\$(nproc)"
if [ "$DPDK_MODE" = af_packet ]; then
  DEV=$(eal_dev) || die "интерфейса $DPDK_IFACE нет -- sudo ./scripts/veth_setup.sh up"
else
  DEV=$(eal_dev) || die "карта не привязана к $DPDK_DRIVER -- sudo ./scripts/nic.sh bind"
fi

# Печатаем адреса той карты, которую слушает DPDK. В pci-режиме ядру она не
# видна, и MAC с IP взять больше неоткуда, кроме метаданных инстанса.
say "Вписать в host.conf отправляющей машины"
if ADDRS=$(dpdk_addrs); then
  row PEER_MAC "$(awk '{print $1}' <<<"$ADDRS")"
  row PEER_IP "$(awk '{print $2}' <<<"$ADDRS")"
else
  bad "IMDS не ответил -- задайте адреса вручную"
fi

clock_check

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

  # После остановки приёмника, а не до: иначе отражатель крутился бы в
  # busy-poll рядом с ним. И в cleanup, а не в конце скрипта, чтобы отработал
  # по Ctrl+C -- отправитель в этот момент как раз снимает второе смещение.
  echo
  clock_reflect "после прогона"
}
trap cleanup EXIT


say "Приём"
row порт "udp/$PORT"
row устройство "$DEV"
[ "$MTU" = 1500 ] || row MTU "$MTU"
row ядра "приёмник $CPU_RECEIVER, consumer $CPU_CONSUMER"
row очередь "RX $RECEIVER_RXD дескрипторов, burst $BURST"
row пул "$RECEIVER_MBUFS mbuf"
row ждём "$COUNT сообщений, прогрев $WARMUP"
[ "$REORDER" = 0 ] || row окно "переупорядочивание на $REORDER кадров"
row логи "$RUN"

clock_reflect "до прогона"

taskset -c "$CPU_RECEIVER" setarch -R "./$RECEIVER_BIN" \
  -l "$CPU_RECEIVER" --file-prefix rx \
  $DEV \
  -- --shm /fanout_ring_rx --slots "$SLOTS" --port "$PORT" --burst "$BURST" \
     --mbufs "$RECEIVER_MBUFS" --rxd "$RECEIVER_RXD" --mtu "$MTU" \
     $REORDER_ARGS \
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
