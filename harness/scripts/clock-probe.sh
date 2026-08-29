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
# Пробник ходит по УПРАВЛЯЮЩЕМУ интерфейсу (ens5), а не по карте под DPDK.
# Смещение -- свойство пары часов, а не пути, поэтому измерять его можно по
# любому каналу. Карту при этом не надо ни отвязывать, ни возвращать: у
# вторичной ENI после возврата ядру всё равно нет адреса, пока ей не отработает
# DHCP, так что тот путь просто не работает.
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/lib.sh"
cd "$ROOT_DIR"

PROBE=${PROBE:-bin/clock_probe}
PROBE_PORT=${PROBE_PORT:-51100}
MGMT_IFACE=${MGMT_IFACE:-ens5}     # интерфейс, по которому ходит пробник
PROBE_PEER=${PROBE_PEER:-}         # адрес второй машины на этом интерфейсе
PROBE_COUNT=${PROBE_COUNT:-20000}
PROBE_IDLE_S=${PROBE_IDLE_S:-90}   # отражатель ждёт столько без трафика и выходит
STATE=${PROBE_STATE_DIR:-/tmp}

name=$(basename "$0")
case "$name" in *start*) PHASE=start ;; *end*) PHASE=end ;;
  *) die "зовите через clock-probe-{start,end}-{rx,tx}.sh" ;; esac
case "$name" in *-rx*) ROLE=rx ;; *-tx*) ROLE=tx ;;
  *) die "зовите через clock-probe-{start,end}-{rx,tx}.sh" ;; esac

[ -x "$PROBE" ] || die "нет $PROBE -- make -j$(nproc)"
mgmt_ip() { ip -4 -o addr show "$MGMT_IFACE" 2>/dev/null | awk '{print $4}' | cut -d/ -f1; }

[ "$ROLE" = rx ] || [ -n "$PROBE_PEER" ] || die "PROBE_PEER пуст.
   Это адрес второй машины на управляющем интерфейсе ($MGMT_IFACE), не на карте
   под DPDK. Посмотреть там: ip -4 -o addr show $MGMT_IFACE
   Вписать в host.conf: PROBE_PEER=<адрес>"

need_root

if [ "$PHASE" = start ]; then
  say "Останавливаю chrony"
  systemctl stop chrony || true
  row chrony "остановлен (вернётся в clock-probe-end-$ROLE.sh)"
fi

# В выводе пробника "p50=" и число разделены пробелами, поэтому по полям awk их
# не собрать -- берём регуляркой по строке. Знак минус обязателен: смещение
# бывает отрицательным, и потерять минус хуже, чем не разобрать вовсе.
parse_offset() {
  sed -nE 's/.*clock offset[[:space:]]+p50=[[:space:]]*(-?[0-9]+).*/\1/p' "$1"
}

if [ "$ROLE" = rx ]; then
  MY_IP=$(mgmt_ip)
  [ -n "$MY_IP" ] || die "не определить адрес $MGMT_IFACE -- задайте MGMT_IFACE=..."
  say "Отражатель ($PHASE)"
  row адрес "$MY_IP:$PROBE_PORT  ($MGMT_IFACE)"
  row выход "через $PROBE_IDLE_S с тишины"
  echo "   теперь на отправителе: sudo ./scripts/clock-probe-$PHASE-tx.sh"
  echo
  "$PROBE" --reflect --bind "$MY_IP" --port "$PROBE_PORT" --idle-s "$PROBE_IDLE_S"
else
  say "Замер смещения ($PHASE)"
  row отражатель "$PROBE_PEER:$PROBE_PORT  ($MGMT_IFACE)"
  row образцов "$PROBE_COUNT"
  echo
  "$PROBE" --probe --peer "$PROBE_PEER:$PROBE_PORT" --count "$PROBE_COUNT" \
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
