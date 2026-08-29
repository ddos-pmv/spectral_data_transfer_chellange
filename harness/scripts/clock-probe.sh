#!/usr/bin/env bash
# Замер смещения часов ДО и ПОСЛЕ прогона. Одно тело, четыре ссылки на него:
#
#   sudo ./scripts/clock-probe-start-rx.sh    приёмник, поднять отражатель
#   sudo ./scripts/clock-probe-start-tx.sh    отправитель, снять смещение
#   ... прогон rx.sh + tx.sh ...
#   sudo ./scripts/clock-probe-end-rx.sh      приёмник, поднять отражатель
#   sudo ./scripts/clock-probe-end-tx.sh      отправитель, снять смещение
#
# Зачем: b = rx_ts - tx_ts снимается разными часами и потому равен настоящему
# времени провода плюс смещение между хостами. Со СТАНОВЛЕННЫМ chrony частотная
# поправка остаётся в ядре, контур её больше не дёргает, и смещение уходит по
# ПРЯМОЙ -- двух замеров хватает, чтобы знать его в любой момент прогона.
# Коррекция: ./scripts/summarize.py latencies.csv --offset-before A --offset-after B
#
# chrony останавливается на ОБЕИХ машинах в фазе start и запускается обратно в
# фазе end. Если оставить его работающим хоть на одной, смещение перестаёт быть
# прямой и линейная коррекция даст неверные числа с видом достоверных.
#
# Карта на время замера возвращается ядру: пробник ходит обычными сокетами, а у
# отданной DPDK карты у ядра нет её адреса. Возврат под DPDK -- через trap,
# чтобы прерванный замер не оставил стенд без карты.
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/lib.sh"
cd "$ROOT_DIR"

PROBE=${PROBE:-bin/clock_probe}
PROBE_PORT=${PROBE_PORT:-51100}
PROBE_COUNT=${PROBE_COUNT:-20000}
PROBE_IDLE_S=${PROBE_IDLE_S:-90}   # отражатель ждёт столько без трафика и выходит
STATE=${PROBE_STATE_DIR:-/tmp}

name=$(basename "$0")
case "$name" in *start*) PHASE=start ;; *end*) PHASE=end ;;
  *) die "зовите через clock-probe-{start,end}-{rx,tx}.sh" ;; esac
case "$name" in *-rx*) ROLE=rx ;; *-tx*) ROLE=tx ;;
  *) die "зовите через clock-probe-{start,end}-{rx,tx}.sh" ;; esac

[ -x "$PROBE" ] || die "нет $PROBE -- make -j$(nproc)"
[ "$ROLE" = rx ] || [ -n "${PEER_IP:-}" ] \
  || die "PEER_IP пуст -- впишите в host.conf адрес приёмника"

need_root

if [ "$PHASE" = start ]; then
  say "Останавливаю chrony"
  systemctl stop chrony || true
  row chrony "остановлен (вернётся в clock-probe-end-$ROLE.sh)"
fi

say "Возвращаю карту ядру на время замера"
trap './scripts/nic.sh bind >/dev/null 2>&1 || true' EXIT
./scripts/nic.sh unbind >/dev/null

# В выводе пробника "p50=" и число разделены пробелами, поэтому по полям awk их
# не собрать -- берём регуляркой по строке. Знак минус обязателен: смещение
# бывает отрицательным, и потерять минус хуже, чем не разобрать вовсе.
parse_offset() {
  sed -nE 's/.*clock offset[[:space:]]+p50=[[:space:]]*(-?[0-9]+).*/\1/p' "$1"
}

if [ "$ROLE" = rx ]; then
  MY_IP=${SRC_IP:-$(local_ip)}
  [ -n "$MY_IP" ] || die "не определить свой IP -- задайте SRC_IP=..."
  say "Отражатель ($PHASE)"
  row адрес "$MY_IP:$PROBE_PORT"
  row выход "через $PROBE_IDLE_S с тишины"
  echo "   теперь на отправителе: sudo ./scripts/clock-probe-$PHASE-tx.sh"
  echo
  "$PROBE" --reflect --bind "$MY_IP" --port "$PROBE_PORT" --idle-s "$PROBE_IDLE_S"
else
  say "Замер смещения ($PHASE)"
  row отражатель "$PEER_IP:$PROBE_PORT"
  row образцов "$PROBE_COUNT"
  echo
  "$PROBE" --probe --peer "$PEER_IP:$PROBE_PORT" --count "$PROBE_COUNT" \
    | tee "$STATE/clock_probe_$PHASE.txt"
  OFF=$(parse_offset "$STATE/clock_probe_$PHASE.txt")
fi

if [ "$PHASE" = end ]; then
  say "Запускаю chrony обратно"
  systemctl start chrony || true
fi

if [ "$ROLE" = tx ]; then
  echo
  say "Смещение ($PHASE)"
  row offset_ns "${OFF:-не разобрал, возьмите из вывода выше}"
  if [ "$PHASE" = end ] && [ -f "$STATE/clock_probe_start.txt" ]; then
    BEF=$(parse_offset "$STATE/clock_probe_start.txt")
    echo
    echo "   ./scripts/summarize.py results/rx-.../latencies.csv \\"
    echo "       --offset-before $BEF --offset-after $OFF"
  fi
fi
