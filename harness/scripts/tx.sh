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
#   sudo PACK=8 ./scripts/tx.sh               # до 8 кадров в датаграмме
#   sudo PACK=8 MIN_PACK=4 ./scripts/tx.sh    # и ДОЖИДАТЬСЯ четырёх
#   sudo MTU=9001 PACK=50 MIN_PACK=16 ./scripts/tx.sh   # jumbo, до 50 кадров
#   sudo DEST_QUEUES=4 ./scripts/tx.sh        # размазать по 4 очередям (нужен REORDER)
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
# Датаграмм за один rte_eth_tx_burst. С PACK это ВТОРАЯ, независимая граница:
# за проход собирается до BURST датаграмм по PACK кадров, то есть до BURST*PACK
# сообщений. Под нагрузкой работают обе, на свободном ходу ни одна.
BURST=${BURST:-32}
COUNT=${COUNT:-0}             # сообщений и выйти; 0 = бесконечно, до Ctrl+C
TX_SPINS=${TX_SPINS:-10}      # ретраев на полном TX-кольце, прежде чем выбросить
# Предел кадров в датаграмме. 1 = каждое сообщение своей датаграммой, как раньше.
#
# ХВОСТ МЕНЬШЕ, КОГДА PACK = MIN_PACK: тогда в каждой датаграмме ровно PACK кадров
# и отрезок a равномерен на фиксированном отрезке. При PACK > MIN_PACK размер
# датаграммы гуляет между ними -- сколько кольцо успело отдать, -- и у a
# появляется второй режим, то есть хвост толще.
# Больше -- батчинг, но только когда кольцо не успевает: упаковывается ровно то,
# что оно отдало за проход, ждать наполнения отправитель не будет. Смотреть после
# прогона на "кадров/дгр": 1.00 значит, что батчинг ни разу не понадобился.
PACK=${PACK:-1}
# Принудительный батчинг: не отправлять датаграмму короче MIN_PACK кадров, даже
# если кольцо пусто. 1 = выключено (по умолчанию), и тогда ожидания нет вовсе.
#
# Это единственное место, где решение платит задержкой за пакеты. Плата
# ограничена PACK_WAIT_SPINS -- холостыми опросами кольца, после которых
# отправляем что набралось. Без предела пауза в потоке стала бы задержкой один
# к одному и легла бы прямо в p99.9. Включать имеет смысл только выше полки
# шейпера, где пакет дороже микросекунды.
MIN_PACK=${MIN_PACK:-1}
# Это ПОТОЛОК НА ОЖИДАНИЕ: оборот стоит ~2.2 нс, значит 10000 оборотов ~ 22 мкс,
# и дольше этого ни один кадр в отрезке a не задержится, каким бы ни был MIN_PACK.
#
# На умолчании предел почти никогда не срабатывает (pack_timeout = 0..1), то есть
# ручка не участвует. Занижение -- скажем, до 1000 (~2.2 мкс) -- включает её и
# ОГРАНИЧИВАЕТ ХВОСТ: датаграммы начинают уходить недобранными, pack_timeout
# растёт, пакетов становится больше. Это обмен пакетной экономии на ограниченный
# хвост, и он осмыслен, когда p99.9/p99.99 важнее пропускной способности.
#
# Ориентир: чтобы добрать N кадров при R сообщений в секунду, нужно ~N/R секунд.
PACK_WAIT_SPINS=${PACK_WAIT_SPINS:-10000}

# Холостых оборотов МЕЖДУ опросами кольца, пока идёт добор до MIN_PACK.
# 0 (по умолчанию) -- опрашивать вплотную, как раньше.
#
# Отправитель делает ~450M опросов в секунду одной кэш-линии -- той, в которую
# producer собирается писать. Сами чтения бесплатны (L1), но в момент записи у
# нас в полёте десятки спекулятивных загрузок этой линии, и её инвалидация
# оборачивается сбросом конвейера. Пауза между опросами это смягчает.
#
# Отправку она не задерживает: момент отправки задаёт producer, выдавший
# последний кадр. Пока PACK_BACKOFF x 0.51 нс заметно меньше наполнения
# (MIN_PACK / RATE), задержка бесплатна. При MIN_PACK=8 и 800k это ~10 мкс,
# то есть разумный диапазон -- десятки, не тысячи.
PACK_BACKOFF=${PACK_BACKOFF:-0}

# TX-очередей на ОДНОГО получателя. 1 = как было: очередь на получателя.
#
# Больше единицы СОЗНАТЕЛЬНО ломает порядок: очереди опустошаются независимо, и
# пакеты одного получателя приходят вперемешку. Приёмник обязан быть запущен с
# окном переупорядочивания (REORDER=N в rx.sh), иначе MonotonicGate посчитает
# перестановку потерей -- на синтетике при разбросе 15 терялось 78% потока.
#
# Зачем это вообще: проверить, ограничивает ли пакетный темп ОДНА очередь ENA
# или инстанс целиком. Нужно n_получателей x DEST_QUEUES очередей на карте,
# иначе отправитель откажется стартовать и напечатает, сколько дала карта.
DEST_QUEUES=${DEST_QUEUES:-1}
if [ "$DEST_QUEUES" = 1 ]; then
  SENDER_BIN=bin/dpdk_sender
  SPREAD_ARGS=
else
  SENDER_BIN=bin/dpdk_sender_mq
  SPREAD_ARGS="--dest-queues $DEST_QUEUES"
fi

# MTU (умолчание в lib.sh). 1500 -- как у карты из коробки, в датаграмму влезает
# 8 кадров (28 + 8*176 = 1436). 9001 -- jumbo внутри VPC, и тогда PACK до 50.
# Ставить надо НА ОБЕИХ сторонах, у приёмника не меньше.
#
# Больше кадров в датаграмме -- меньше пакетов, но дольше её наполнять, и это
# ожидание целиком ложится в отрезок a: a_p50 ~ MIN_PACK / (2 x RATE).

# Проверяем до need_root: с опечаткой в конфиге незачем даже спрашивать пароль.
need_uint SENDER_TXD "$SENDER_TXD" 64 4096
need_pow2 SENDER_TXD "$SENDER_TXD"
need_uint SENDER_MBUFS "$SENDER_MBUFS" 1024 1048576
need_uint BURST "$BURST" 1 512
need_uint SLOTS "$SLOTS" 8 1048576
need_pow2 SLOTS "$SLOTS"
need_uint COUNT "$COUNT" 0 100000000000
need_uint TX_SPINS "$TX_SPINS" 1 1000
# Предел PACK задаёт MTU, а не константа: (MTU - 20 IPv4 - 8 UDP) / 176 байт на
# кадр. При 1500 это 8, при 9001 -- 50. Отправитель проверит ещё раз, уже против
# MTU, который реально дала карта, и зажмёт при расхождении.
need_uint MTU "$MTU" 576 9216
PACK_MAX=$(( (MTU - 28) / 176 ))
[ "$PACK_MAX" -ge 1 ] || die "MTU=$MTU мал даже для одного кадра (176 байт)"
[ "$PACK_MAX" -gt 64 ] && PACK_MAX=64   # kMaxPack -- граница массивов в отправителе
need_uint PACK "$PACK" 1 "$PACK_MAX"
need_uint MIN_PACK "$MIN_PACK" 1 "$PACK"
need_uint PACK_WAIT_SPINS "$PACK_WAIT_SPINS" 1 10000000
need_uint PACK_BACKOFF "$PACK_BACKOFF" 0 100000
need_uint DEST_QUEUES "$DEST_QUEUES" 1 8

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
[ -x "$SENDER_BIN" ] || die "нет $SENDER_BIN -- make -j\$(nproc)"
clock_check
if clock_enabled && [ -z "$PROBE_PEERS" ]; then
  die "PROBE_PEERS пуст -- адреса приёмников на управляющем интерфейсе.
   Посмотреть там: ip -4 -o addr show; вписать в host.conf
   PROBE_PEERS=\"IP1 IP2 ...\" в том же порядке, что и PEERS.
   Либо CLOCK=0, чтобы не мерить смещение."
fi

if [ "$DPDK_MODE" = af_packet ]; then
  DEV=$(eal_dev) || die "интерфейса $DPDK_IFACE нет -- sudo ./scripts/veth_setup.sh up"
else
  DEV=$(eal_dev) || die "карта не привязана к $DPDK_DRIVER -- sudo ./scripts/nic.sh bind"
fi
SRC_IP=${SRC_IP:-$(local_ip)}
[ -n "$SRC_IP" ] || die "не определить свой IP -- задайте SRC_IP=..."

# Чужой отправитель, переживший Ctrl+C прошлого прогона, удваивает нагрузку:
# пакеты упираются в PPS-лимит ENI, и в замерах появляются потери и хвост в мс.
STALE=$(pgrep -x 'producer|dpdk_sender|dpdk_sender_mq' | tr '\n' ' ' || true)
[ -z "$STALE" ] || die "уже запущены producer/отправитель (pid: $STALE) -- добейте их"

COUNT_ARG=()
[ "$COUNT" = 0 ] || COUNT_ARG=(--count "$COUNT")

RUN=results/tx-$(date +%Y%m%d-%H%M%S)
mkdir -p "$RUN"
rm -f /dev/shm/fanout_ring

OFF_BEFORE=()
AT_BEFORE=()
if clock_enabled; then
  say "Смещение часов до прогона"
  i=0
  while read -r peer off at; do
    [ -n "$off" ] || die "не удалось снять смещение до $peer -- запущен ли там rx.sh?
   Он поднимает отражатель. См. $RUN/clock.log"
    OFF_BEFORE[$i]=$off
    AT_BEFORE[$i]=$at
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
    local i=0 peer after at
    while read -r peer after at; do
      if [ -n "$after" ]; then
        row "$peer" "$after нс, за прогон $(( after - OFF_BEFORE[i] )) нс"
        # Моменты замеров идут вместе со смещениями: поправка привязывается к ним,
        # а не растягивается по первому и последнему отсчёту. Иначе наклон завышен
        # на всё время, что прошло от замера до первого отсчёта и от последнего до
        # второго замера, -- на быстро уходящих часах это уводит b под ноль.
        echo "      там: ./scripts/summarize.py results/rx-*/latencies.csv \\"
        echo "           --offset ${OFF_BEFORE[i]} --offset-at ${AT_BEFORE[i]} \\"
        echo "           --offset-end $after --offset-end-at $at"
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
[ "$MTU" = 1500 ] || row MTU "$MTU"
row ядра "producer $CPU_PRODUCER, отправитель $CPU_SENDER"
row темп "$RATE msg/s, тип $TYPE"
row очередь "TX $SENDER_TXD дескрипторов, burst $BURST датаграмм, ретраев $TX_SPINS"
[ "$PACK" = 1 ] || row упаковка "до $PACK кадров в датаграмме, до $((BURST * PACK)) сообщений за проход"
[ "$DEST_QUEUES" = 1 ] || row размазывание "$DEST_QUEUES очередей на получателя -- ПОРЯДОК ЛОМАЕТСЯ, нужен REORDER на приёмнике"
[ "$PACK_BACKOFF" = 0 ] || row пауза "$PACK_BACKOFF оборотов между опросами кольца (~$((PACK_BACKOFF / 2)) нс)"
[ "$MIN_PACK" = 1 ] || row ждать "не меньше $MIN_PACK кадров, предел $PACK_WAIT_SPINS оборотов"
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
taskset -c "$CPU_SENDER" setarch -R "./$SENDER_BIN" \
  -l "$CPU_SENDER" --file-prefix tx \
  $DEV \
  -- --shm /fanout_ring --slots "$SLOTS" \
     --src-ip "$SRC_IP" "${DEST_ARGS[@]}" --burst "$BURST" \
     --mbufs "$SENDER_MBUFS" --txd "$SENDER_TXD" --tx-spins "$TX_SPINS" \
     --pack "$PACK" --min-pack "$MIN_PACK" --mtu "$MTU" \
     $SPREAD_ARGS \
     --pack-wait-spins "$PACK_WAIT_SPINS" --pack-backoff "$PACK_BACKOFF" \
     "${COUNT_ARG[@]}" \
  > "$RUN/sender.log" 2>&1 &
SENDER=$!

[ "$COUNT" = 0 ] && echo "   идёт отправка, Ctrl+C для остановки" \
  || echo "   идёт отправка, $COUNT сообщений"
wait "$SENDER" 2>/dev/null || true
