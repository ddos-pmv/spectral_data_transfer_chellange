#!/usr/bin/env bash
# Отправляющая сторона: producer -> shm -> dpdk_sender -> сеть.
#
# Запускать ВТОРЫМ, после rx.sh на приёмной машине.
# Останавливать Ctrl+C: оба процесса допечатают статистику.
#
#   sudo ./scripts/tx.sh
#   sudo RATE=500000 BURST=8 ./scripts/tx.sh
#
# Адрес получателя (PEER_MAC/PEER_IP) берётся из scripts/host.conf -- его
# печатает rx.sh на приёмной машине.
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/lib.sh"
cd "$ROOT_DIR"

RATE=${RATE:-200000}          # msg/s, 0 = без троттлинга
TYPE=${TYPE:-mixed}           # trade | bbo | book | mixed
SLOTS=${SLOTS:-1024}
BURST=${BURST:-32}

need_root
[ -x bin/dpdk_sender ] || die "нет bin/dpdk_sender -- make -j\$(nproc)"
[ -n "$PEER_MAC" ] || die "PEER_MAC пуст: запустите rx.sh на приёмной машине и впишите в host.conf"
[ -n "$PEER_IP" ]  || die "PEER_IP пуст: там же"

DEV=$(eal_dev) || die "карта не привязана к $DPDK_DRIVER -- sudo ./scripts/nic.sh bind
   (для локального стенда на veth задайте EAL_DEV=--vdev net_af_xdp0,iface=...)"
SRC_IP=${SRC_IP:-$(local_ip)}
[ -n "$SRC_IP" ] || die "не определить свой IP -- задайте SRC_IP=..."

# Чужой отправитель, переживший Ctrl+C прошлого прогона, удваивает нагрузку:
# пакеты упираются в PPS-лимит ENI, и в замерах появляются потери и хвост в мс.
STALE=$(pgrep -x 'producer|dpdk_sender' | tr '\n' ' ' || true)
[ -z "$STALE" ] || die "уже запущены producer/dpdk_sender (pid: $STALE) -- добейте их"

RUN=results/tx-$(date +%Y%m%d-%H%M%S)
mkdir -p "$RUN"
rm -f /dev/shm/fanout_ring

PRODUCER=
SENDER=
cleanup() {
  # Отправителя гасим первым и дожидаемся: по SIGTERM он допечатывает статистику.
  [ -n "$SENDER" ] && kill -TERM "$SENDER" 2>/dev/null || true
  [ -n "$SENDER" ] && wait "$SENDER" 2>/dev/null || true
  [ -n "$PRODUCER" ] && kill -TERM "$PRODUCER" 2>/dev/null || true
  [ -n "$PRODUCER" ] && wait "$PRODUCER" 2>/dev/null || true
  rm -f /dev/shm/fanout_ring
  [ -n "${SUDO_UID:-}" ] && chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$RUN" || true
  echo
  echo "статистика отправителя: $RUN/sender.log"
  tail -n 9 "$RUN/sender.log" 2>/dev/null || true
}
trap cleanup EXIT

say "Отправка"
row маршрут "$SRC_IP -> $PEER_IP:$PORT"
row "MAC получателя" "$PEER_MAC"
row устройство "$DEV"
row ядра "producer $CPU_PRODUCER, отправитель $CPU_SENDER"
row темп "$RATE msg/s, тип $TYPE"
row логи "$RUN"

taskset -c "$CPU_PRODUCER" setarch -R ./bin/producer \
  --shm /fanout_ring --slots "$SLOTS" --count 0 --rate "$RATE" --type "$TYPE" \
  > "$RUN/producer.log" 2>&1 &
PRODUCER=$!

# Кольцо должно существовать до старта отправителя: он открывает его с create=false.
for _ in $(seq 50); do [ -e /dev/shm/fanout_ring ] && break; sleep 0.1; done
[ -e /dev/shm/fanout_ring ] || die "producer не создал кольцо, см. $RUN/producer.log"

# В файл и фоном, а не в конвейер: по Ctrl+C сигнал прилетает всей группе
# процессов переднего плана, tee умирает первым, и статистика отправителя
# уходит в закрытую трубу -- SIGPIPE убивает его на середине вывода.
taskset -c "$CPU_SENDER" setarch -R ./bin/dpdk_sender \
  -l "$CPU_SENDER" --file-prefix tx \
  $DEV \
  -- --shm /fanout_ring --slots "$SLOTS" \
     --dst-mac "$PEER_MAC" --src-ip "$SRC_IP" --dst-ip "$PEER_IP" \
     --dst-port "$PORT" --burst "$BURST" \
  > "$RUN/sender.log" 2>&1 &
SENDER=$!

echo "   идёт отправка, Ctrl+C для остановки"
wait "$SENDER" 2>/dev/null || true
