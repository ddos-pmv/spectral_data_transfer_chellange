#!/usr/bin/env bash
# Отправляющая сторона: producer -> shm -> dpdk_sender -> сеть.
#
# Запускать ВТОРЫМ, после rx.sh на приёмной машине.
# Останавливать Ctrl+C: оба процесса допечатают статистику.
#
#   sudo ./scripts/tx.sh
#   sudo RATE=500000 BURST=8 ./scripts/tx.sh
#   sudo RECEIVERS=2 ./scripts/tx.sh          # только первые 2 из PEERS
#   sudo COUNT=5000000 ./scripts/tx.sh        # выйти самому, без Ctrl+C
#   sudo TX_SPINS=1 ./scripts/tx.sh           # не крутиться на полном TX-кольце
#   sudo CLOCK=0 ./scripts/tx.sh              # без замера смещения часов
#
# Замер смещения требует PROBE_PEERS в host.conf -- адреса приёмников на
# управляющем интерфейсе, по одному на каждого, в том же порядке, что PEERS.
# rx.sh на каждой приёмной машине поднимает свой отражатель сам, а смещение
# снимается до каждого отдельно: часы у машин расходятся независимо, и одна
# общая поправка на веер была бы средним по разным расхождениям.
#
# Размеры TX-очереди и пула mbuf -- SENDER_TXD/SENDER_MBUFS в host.conf.
#
# Получатели берутся из scripts/host.conf: PEERS="MAC@IP:PORT ..." для веера,
# или пара PEER_MAC/PEER_IP для одного -- её печатает rx.sh на приёмной машине.
# RECEIVERS=N берёт первые N из этого списка, не трогая конфиг: развёртка по
# числу получателей делается перебором одной переменной, а не правкой файла
# между прогонами (иначе в результатах не докажешь, что менялось только оно).
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/lib.sh"
cd "$ROOT_DIR"

RATE=${RATE:-200000}          # msg/s, 0 = без троттлинга
TYPE=${TYPE:-mixed}           # trade | bbo | book | mixed
SLOTS=${SLOTS:-1024}
BURST=${BURST:-32}
COUNT=${COUNT:-0}             # сообщений и выйти; 0 = бесконечно, до Ctrl+C
TX_SPINS=${TX_SPINS:-10}      # ретраев на полном TX-кольце, прежде чем выбросить

# Проверяем до need_root: с опечаткой в конфиге незачем даже спрашивать пароль.
need_uint SENDER_TXD "$SENDER_TXD" 64 4096
need_pow2 SENDER_TXD "$SENDER_TXD"
need_uint SENDER_MBUFS "$SENDER_MBUFS" 1024 1048576
need_uint BURST "$BURST" 1 512
need_uint SLOTS "$SLOTS" 8 1048576
need_pow2 SLOTS "$SLOTS"
need_uint COUNT "$COUNT" 0 100000000000
need_uint TX_SPINS "$TX_SPINS" 1 1000

# Получатели. Форма адресации ровно одна -- список MAC@IP:PORT; пара
# PEER_MAC/PEER_IP сворачивается в него здесь же. Дальше ни скрипт, ни сам
# отправитель не знают, что "один получатель" бывает особым случаем.
if [ -z "${PEERS:-}" ]; then
  [ -n "${PEER_MAC:-}" ] || die "получатели не заданы: впишите в host.conf PEER_MAC/PEER_IP
   (их печатает rx.sh на приёмной машине) или PEERS=\"MAC@IP:PORT ...\" для нескольких"
  [ -n "${PEER_IP:-}" ]  || die "PEER_IP пуст: там же"
  PEERS="$PEER_MAC@$PEER_IP:$PORT"
fi

# RECEIVERS=N -- взять первые N получателей из PEERS. Порядок в списке при этом
# и есть порядок назначений в отправителе, то есть dest 0 при N=1 и при N=4 --
# один и тот же получатель, и его числа между прогонами сравнимы.
if [ -n "${RECEIVERS:-}" ]; then
  need_uint RECEIVERS "$RECEIVERS" 1 16
  ALL_PEERS=($PEERS)
  [ "$RECEIVERS" -le "${#ALL_PEERS[@]}" ] \
    || die "RECEIVERS=$RECEIVERS, а в PEERS перечислено ${#ALL_PEERS[@]}"
  PEERS="${ALL_PEERS[*]:0:$RECEIVERS}"
fi

# Массив, а не строка: MAC-адреса не должны разъехаться при словоделении.
DEST_ARGS=()
for peer in $PEERS; do
  case "$peer" in
    *@*:*) DEST_ARGS+=(--dest "$peer") ;;
    *) die "получатель '$peer' не в формате MAC@IP:PORT" ;;
  esac
done
NDEST=$(( ${#DEST_ARGS[@]} / 2 ))

# Смещение часов меряется до КАЖДОГО приёмника: у каждой машины свои часы, и одна
# общая поправка на веер смешала бы разные расхождения в одну цифру. Порядок в
# PROBE_PEERS -- тот же, что в PEERS, поэтому RECEIVERS=N режет оба списка одинаково.
PROBE_LIST=($PROBE_PEERS)
if clock_enabled; then
  [ "${#PROBE_LIST[@]}" -ge "$NDEST" ] || die "получателей $NDEST, а в PROBE_PEERS адресов ${#PROBE_LIST[@]}.
   Впишите в host.conf PROBE_PEERS=\"IP1 IP2 ...\" -- адреса приёмников на управляющем интерфейсе,
   в том же порядке, что и в PEERS. Либо CLOCK=0, чтобы не мерить смещение."
  PROBE_LIST=("${PROBE_LIST[@]:0:$NDEST}")
fi

check_pool SENDER_MBUFS "$SENDER_MBUFS" SENDER_TXD "$SENDER_TXD" "$BURST" "$NDEST"

need_root
[ -x bin/dpdk_sender ] || die "нет bin/dpdk_sender -- make -j\$(nproc)"
clock_check
if clock_enabled && [ -z "$PROBE_PEERS" ]; then
  die "PROBE_PEERS пуст -- адреса приёмников на управляющем интерфейсе.
   Посмотреть там: ip -4 -o addr show; вписать в host.conf
   PROBE_PEERS=\"IP1 IP2 ...\" в том же порядке, что и PEERS.
   Либо CLOCK=0, чтобы не мерить смещение."
fi

DEV=$(eal_dev) || die "карта не привязана к $DPDK_DRIVER -- sudo ./scripts/nic.sh bind"
SRC_IP=${SRC_IP:-$(local_ip)}
[ -n "$SRC_IP" ] || die "не определить свой IP -- задайте SRC_IP=..."

# Чужой отправитель, переживший Ctrl+C прошлого прогона, удваивает нагрузку:
# пакеты упираются в PPS-лимит ENI, и в замерах появляются потери и хвост в мс.
STALE=$(pgrep -x 'producer|dpdk_sender' | tr '\n' ' ' || true)
[ -z "$STALE" ] || die "уже запущены producer/dpdk_sender (pid: $STALE) -- добейте их"

COUNT_ARG=()
[ "$COUNT" = 0 ] || COUNT_ARG=(--count "$COUNT")

RUN=results/tx-$(date +%Y%m%d-%H%M%S)
mkdir -p "$RUN"
rm -f /dev/shm/fanout_ring

OFF_BEFORE=()
if clock_enabled; then
  say "Смещение часов до прогона"
  i=0
  while read -r peer off; do
    [ -n "$off" ] || die "не удалось снять смещение до $peer -- запущен ли там rx.sh?
   Он поднимает отражатель. См. $RUN/clock.log"
    OFF_BEFORE[$i]=$off
    row "$peer" "$off нс"
    i=$((i + 1))
  done < <(clock_probe_all "$RUN/clock.log" "${PROBE_LIST[@]}")
  [ "$i" = "$NDEST" ] || die "снято смещений: $i из $NDEST, см. $RUN/clock.log"
  # Отражатели выходят через PROBE_IDLE_S тишины, и только после этого rx.sh на
  # той стороне переходит к самому прогону. Ждём последнего из них, иначе первые
  # пакеты уйдут в ещё не поднятый приёмник.
  sleep $((PROBE_IDLE_S + 2))
fi

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
  tail -n 20 "$RUN/sender.log" 2>/dev/null || true

  # После прогона, а не после wait: останавливают tx.sh обычно по Ctrl+C.
  if clock_enabled && [ "${#OFF_BEFORE[@]}" != 0 ]; then
    echo
    say "Смещение часов после прогона"
    local i=0 peer after
    while read -r peer after; do
      if [ -n "$after" ]; then
        row "$peer" "$after нс, за прогон $(( after - OFF_BEFORE[i] )) нс"
        echo "      там: ./scripts/summarize.py results/rx-*/latencies.csv \\"
        echo "           --offset ${OFF_BEFORE[i]} --offset-end $after"
      else
        row "$peer" "замер не удался, см. $RUN/clock.log"
      fi
      i=$((i + 1))
    done < <(clock_probe_all "$RUN/clock.log" "${PROBE_LIST[@]}")
  fi
}
trap cleanup EXIT

say "Отправка"
row отправитель "$SRC_IP"
row получатели "$NDEST"
for peer in $PEERS; do row "  ->" "$peer"; done
row устройство "$DEV"
row ядра "producer $CPU_PRODUCER, отправитель $CPU_SENDER"
row темп "$RATE msg/s, тип $TYPE"
row очередь "TX $SENDER_TXD дескрипторов, burst $BURST, ретраев $TX_SPINS"
row пул "$SENDER_MBUFS mbuf"
[ "$COUNT" = 0 ] || row остановка "после $COUNT сообщений"
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
     --src-ip "$SRC_IP" "${DEST_ARGS[@]}" --burst "$BURST" \
     --mbufs "$SENDER_MBUFS" --txd "$SENDER_TXD" --tx-spins "$TX_SPINS" \
     "${COUNT_ARG[@]}" \
  > "$RUN/sender.log" 2>&1 &
SENDER=$!

[ "$COUNT" = 0 ] && echo "   идёт отправка, Ctrl+C для остановки" \
  || echo "   идёт отправка, $COUNT сообщений"
wait "$SENDER" 2>/dev/null || true
