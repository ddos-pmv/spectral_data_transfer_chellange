# Общая часть всех host-скриптов. Подключается через `.`, сама ничего не делает.
#
# Даёт: чтение host.conf, значения по умолчанию, вывод, root-проверку,
# разрешение PCI-адреса и чтение адресов ENI из метаданных инстанса.

# Каталог скриптов, независимо от того, откуда позвали.
LIB_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(dirname "$LIB_DIR")

if [ -f "$LIB_DIR/host.conf" ]; then
  # Окружение сильнее файла -- но host.conf присваивает без :-, поэтому простой
  # `. host.conf` затирал бы переменные, заданные в командной строке. Тихо: ни
  # ошибки, ни предупреждения, просто прогон идёт не с теми числами, которые вы
  # написали. Поэтому: запоминаем всё, что уже задано и упоминается в файле,
  # читаем файл, возвращаем запомненное на место.
  __hc_saved=()
  while read -r __hc_key; do
    [ -n "${!__hc_key+set}" ] && __hc_saved+=("$(declare -p "$__hc_key")")
  done < <(sed -nE 's/^[[:space:]]*([A-Za-z_][A-Za-z0-9_]*)=.*/\1/p' "$LIB_DIR/host.conf" | sort -u)

  # shellcheck disable=SC1091
  . "$LIB_DIR/host.conf"

  for __hc_decl in ${__hc_saved+"${__hc_saved[@]}"}; do eval "$__hc_decl"; done
  unset __hc_saved __hc_decl __hc_key
elif [ -z "${HOSTCONF_WARNED:-}" ]; then
  # host.sh зовёт четыре скрипта подряд, каждый подключает lib.sh -- ругаемся один раз.
  export HOSTCONF_WARNED=1
  echo "нет scripts/host.conf -- беру значения по умолчанию" >&2
  echo "  cp scripts/host.conf.example scripts/host.conf" >&2
fi

# Значения по умолчанию ставим ПОСЛЕ конфига и через :-, поэтому файл
# перебивает эти дефолты. Приоритет целиком: окружение > host.conf > дефолт.
DPDK_IFACE=${DPDK_IFACE:-enp40s0}
DPDK_PCI=${DPDK_PCI:-}
DPDK_ENI_INDEX=${DPDK_ENI_INDEX:-1}
DPDK_DRIVER=${DPDK_DRIVER:-igb_uio}
KERNEL_DRIVER=${KERNEL_DRIVER:-ena}
ISOL_CPUS=${ISOL_CPUS:-}
IRQ_CPUS=${IRQ_CPUS:-0}
OFFLINE_CPUS=${OFFLINE_CPUS:-}
GOVERNOR=${GOVERNOR:-}
NO_TURBO=${NO_TURBO:-}
HUGEPAGES_2M=${HUGEPAGES_2M:-256}
CPU_RECEIVER=${CPU_RECEIVER:-1}
CPU_CONSUMER=${CPU_CONSUMER:-2}
CPU_SENDER=${CPU_SENDER:-1}
CPU_PRODUCER=${CPU_PRODUCER:-2}
PORT=${PORT:-40001}
PEER_MAC=${PEER_MAC:-}
PEER_IP=${PEER_IP:-}

# Очереди NIC и пулы mbuf. Отправитель и приёмник разведены по именам: на
# локальном стенде обе стороны читают один host.conf.
SENDER_MBUFS=${SENDER_MBUFS:-8191}
SENDER_TXD=${SENDER_TXD:-1024}
RECEIVER_MBUFS=${RECEIVER_MBUFS:-8191}
RECEIVER_RXD=${RECEIVER_RXD:-1024}

STATE_FILE=$LIB_DIR/.host-state
export PATH=$PATH:/opt/src/dpdk-stable/usertools

# ------------------------------------------------------------------- вывод
say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
# printf считает ширину в байтах, а кириллица в UTF-8 двухбайтовая, поэтому
# %-24s ломает колонки. ${#1} в bash считает символы -- добиваем вручную.
pad()  { printf '%s%*s' "$1" $(($2 - ${#1})) ""; }
row()  { printf '   %s%s\n' "$(pad "$1" 25)" "$2"; }
ok()   { printf '   \033[32mOK\033[0m   %s\n' "$*"; }
warn() { printf '   \033[33mВНИМ\033[0m %s\n' "$*"; }
bad()  { printf '   \033[31mНЕТ\033[0m  %s\n' "$*"; }
die()  { echo "$*" >&2; exit 1; }

need_root() { [ "$(id -u)" = 0 ] || die "нужен root: sudo $0 $*"; }

# ---------------------------------------------------------------- проверки
# Опечатка в числе не должна доезжать до EAL: там она превращается либо в
# молчаливое усечение до uint16, либо в невнятную ошибку из недр драйвера.
is_uint() { case ${1:-} in '' | *[!0-9]*) return 1 ;; *) return 0 ;; esac; }

need_uint() {  # need_uint ИМЯ ЗНАЧЕНИЕ МИН МАКС
  is_uint "$2" || die "$1='$2' -- нужно целое неотрицательное число"
  [ "$2" -ge "$3" ] && [ "$2" -le "$4" ] || die "$1=$2 вне диапазона $3..$4"
}

# Дескрипторы очереди у ENA и большинства PMD -- только степень двойки; всё
# прочее драйвер молча округляет вниз, и в замерах оказывается не тот размер,
# который вписан в конфиг.
need_pow2() {
  [ $(( $2 & ($2 - 1) )) -eq 0 ] || die "$1=$2 -- нужна степень двойки (256, 512, 1024, 2048, 4096)"
}

# Пул должен покрывать всю очередь разом: дескрипторы держат mbuf до тех пор,
# пока NIC не отчитается о завершении. Плюс burst в полёте и per-lcore кэш
# mempool (cache_size=256 в коде) -- эти mbuf в общий оборот не возвращаются.
check_pool() {  # check_pool ИМЯ_ПУЛА ПУЛ ИМЯ_ОЧЕРЕДИ ДЕСКРИПТОРЫ BURST [ПОЛУЧАТЕЛЕЙ]
  # Веер множит расход дважды: отправитель держит свой burst mbuf-ов на каждого
  # получателя, и каждому получателю достаётся своя TX-очередь -- то есть ещё по
  # очереди дескрипторов. Отправитель проверяет то же самое сам и не стартует с
  # маленьким пулом; здесь -- чтобы узнать об этом до пароля sudo.
  local dests=${6:-1}
  local need=$(( $4 * dests + $5 * dests * 2 + 256 ))
  [ "$2" -gt "$need" ] || die "$1=$2 мало при $3=$4, BURST=$5 и получателях=$dests: нужно больше $need
   ($dests x очередь $4 + $dests x burst в полёте + кэш mempool 256)
   возьмите $1=$(( ($4 * dests) * 2 + 4095 ))"
}

# Единая обработка подкоманд. status -- по умолчанию, поэтому голый запуск
# любого скрипта ничего не меняет.
dispatch() {
  local cmd=${1:-status}
  case $cmd in
    status|apply|revert|bind|unbind) "cmd_$cmd" ;;
    -h|--help) awk 'NR > 1 && /^#/ {sub(/^# ?/, ""); print; next} NR > 1 {exit}' "$0" ;;
    *) die "не знаю команду '$cmd'; есть: $SUBCOMMANDS" ;;
  esac
}

# ----------------------------------------------------------------- метаданные
# Карту, отданную DPDK, ядро больше не видит: имени интерфейса нет, MAC и IP
# взять неоткуда. Остаётся IMDS -- он знает все ENI инстанса.
IMDS_TOKEN=""
imds() {
  [ -n "$IMDS_TOKEN" ] || IMDS_TOKEN=$(curl -s -m 2 -X PUT \
      -H 'X-aws-ec2-metadata-token-ttl-seconds: 300' \
      http://169.254.169.254/latest/api/token) || return 1
  curl -s -m 2 -H "X-aws-ec2-metadata-token: $IMDS_TOKEN" \
      "http://169.254.169.254/latest/meta-data/$1"
}

# Печатает "MAC IP" для ENI с заданным device-number.
eni_addrs() {
  local want=${1:-$DPDK_ENI_INDEX} mac
  for mac in $(imds network/interfaces/macs/ 2>/dev/null); do
    mac=${mac%/}
    [ "$(imds "network/interfaces/macs/$mac/device-number")" = "$want" ] || continue
    echo "$mac $(imds "network/interfaces/macs/$mac/local-ipv4s" | head -1)"
    return 0
  done
  return 1
}

# ------------------------------------------------------------------- PCI
# Пока карта у ядра, адрес виден по симлинку интерфейса. После привязки к
# igb_uio интерфейс исчезает -- тогда работает только DPDK_PCI из конфига или
# разбор вывода devbind.
resolve_pci() {
  if [ -n "$DPDK_PCI" ]; then echo "$DPDK_PCI"; return 0; fi
  if [ -e "/sys/class/net/$DPDK_IFACE/device" ]; then
    basename "$(readlink -f "/sys/class/net/$DPDK_IFACE/device")"
    return 0
  fi
  # Интерфейса нет -- значит карта уже у DPDK. Ищем среди устройств драйвера.
  local d
  for d in "/sys/bus/pci/drivers/$DPDK_DRIVER"/0000:*; do
    [ -e "$d" ] || continue
    basename "$d"
    return 0
  done
  return 1
}

# Драйвер, за которым сейчас числится устройство.
pci_driver() {
  local pci=$1
  [ -e "/sys/bus/pci/devices/$pci/driver" ] || { echo "-"; return; }
  basename "$(readlink -f "/sys/bus/pci/devices/$pci/driver")"
}

# Список CPU из "1,2" или "1-3" в "1 2 3".
expand_cpus() {
  local spec=$1 part a b out=""
  [ -n "$spec" ] || return 0
  for part in ${spec//,/ }; do
    case $part in
      *-*) a=${part%-*}; b=${part#*-}; out="$out $(seq "$a" "$b")" ;;
      *)   out="$out $part" ;;
    esac
  done
  echo $out
}

# ---------------------------------------------------------------- EAL
# Аргумент устройства для EAL: карта адресуется по PCI, и только так. Никакой
# подстановки другого драйвера тут быть не должно -- если карта не отдана DPDK,
# правильный ответ "карта не привязана", а не молчаливый переход в режим, в
# котором числа означают что-то другое.
#
# llq_policy=3 -- large LLQ: запись очереди 256 байт вместо 128, из них 224
# доступны под inline. Пакет этого транспорта -- 42 байта заголовков плюс не
# более 176 байт кадра, итого 218, то есть влезает целиком. Карта забирает его
# записью в свою память по MMIO и НЕ идёт DMA-чтением за телом в память хоста;
# именно этот сэкономленный round-trip и есть основной выигрыш ENA по хвосту.
# Формат сообщения в message.h ужат до 176 байт ровно под этот порог.
#
# Плата за это -- вдвое более мелкое TX-кольцо (карта отдаёт столько же памяти
# под вдвое более крупные записи). Отсюда SENDER_TXD=256..512, а не 2048; если
# отправитель на старте скажет "TX descriptors: requested X, applyed Y" --
# причина в этом, и в host.conf надо вписать Y.
LLQ_POLICY=${LLQ_POLICY:-3}
eal_dev() {
  local pci; pci=$(resolve_pci 2>/dev/null) || pci=""
  [ -n "$pci" ] || return 1
  [ "$(pci_driver "$pci")" = "$DPDK_DRIVER" ] || return 1
  if [ "$LLQ_POLICY" = "off" ]; then echo "-a $pci"; else echo "-a $pci,llq_policy=$LLQ_POLICY"; fi
}

# IP той карты, через которую реально идёт трафик. VPC режет пакеты, у которых
# src IP не совпадает с адресом ENI, так что брать адрес с соседней нельзя.
# Карта отдана DPDK, ядро её не видит -- адрес берётся только из IMDS.
local_ip() {
  eni_addrs "$DPDK_ENI_INDEX" 2>/dev/null | awk '{print $2}'
}

# ---------------------------------------------------------------- igb_uio
IGB_UIO_CONF=/etc/modprobe.d/igb_uio.conf

# Загрузить igb_uio так, чтобы wc_activate=1 точно применился.
#
# Без Write Combining LLQ пишет дескрипторы в память устройства отдельными
# несливаемыми транзакциями -- ради этого igb_uio и берётся вместо vfio-pci.
# Проверить постфактум нельзя: wc_activate объявлен с правами 0 и в
# /sys/module/igb_uio/parameters/ не появляется. Значит гарантировать надо
# в момент загрузки, а уже загруженному модулю параметр не подсунуть.
ensure_igb_uio() {
  local want="options igb_uio wc_activate=1"
  if [ "$(cat "$IGB_UIO_CONF" 2>/dev/null)" != "$want" ]; then
    echo "$want" > "$IGB_UIO_CONF" || return 1
    ok "записан $IGB_UIO_CONF"
  fi
  modprobe uio

  if ! grep -q '^igb_uio ' /proc/modules; then
    modprobe igb_uio wc_activate=1 || return 1
    ok "igb_uio загружен с wc_activate=1"
    return 0
  fi

  # Уже загружен, и с каким параметром -- неизвестно. Если к нему ничего не
  # привязано, перезагружаем начисто. Если привязано -- трогать нельзя.
  local bound
  bound=$(ls /sys/bus/pci/drivers/igb_uio/ 2>/dev/null | grep -c '^0000:' || true)
  if [ "${bound:-0}" = 0 ]; then
    if rmmod igb_uio 2>/dev/null && modprobe igb_uio wc_activate=1; then
      ok "igb_uio перезагружен с wc_activate=1"
      return 0
    fi
  fi
  warn "igb_uio уже загружен (устройств привязано: ${bound:-0}), параметр не проверить"
  echo "     гарантировать WC: sudo ./scripts/nic.sh unbind && sudo rmmod igb_uio,"
  echo "     затем sudo ./scripts/nic.sh bind"
  return 0
}

# ------------------------------------------------------ смещение часов
# Прогон обрамляется замерами смещения: rx.sh поднимает отражатель, tx.sh к нему
# ходит. Смещение снимается ДО и ПОСЛЕ, потому что при остановленном chrony часы
# расходятся линейно, и по двум точкам поправка восстанавливается для каждого
# отсчёта. Отключается CLOCK=0.
#
# chrony останавливается и обратно НЕ включается: он должен стоять всю серию,
# иначе его поправки каждые 2 с делают смещение непредсказуемым.
# Вернуть после серии: sudo systemctl start chrony
CLOCK=${CLOCK:-1}
PROBE=${PROBE:-bin/clock_probe}
PROBE_PORT=${PROBE_PORT:-51100}
PROBE_COUNT=${PROBE_COUNT:-20000}
PROBE_IDLE_S=${PROBE_IDLE_S:-6}    # отражатель ждёт столько без трафика и выходит
# Сколько отражатель ждёт ПЕРВОГО пакета. При веере отправитель обходит приёмников по
# одному, и последний ждёт всех предыдущих замеров: 20000 пинг-понгов на 5000/с -- это
# 4 с на приёмника. Ста секунд хватает на любой разумный веер, а простаивает отражатель
# только на стенде, где всё равно ничего больше не идёт.
PROBE_WAIT_S=${PROBE_WAIT_S:-100}
# tx: адреса приёмников на управляющем интерфейсе, в том же порядке, что и PEERS.
# PROBE_PEER -- частный случай на одного; ниже он сворачивается в список.
PROBE_PEERS=${PROBE_PEERS:-${PROBE_PEER:-}}
PROBE_PEER=${PROBE_PEER:-}
# Управляющий интерфейс. Пусто -- взять тот, через который идёт маршрут по
# умолчанию: имя зависит от размера инстанса, и зашитый ens5 на c6in.xlarge
# просто не существует.
MGMT_IFACE=${MGMT_IFACE:-}

clock_enabled() { [ "$CLOCK" = 1 ]; }

clock_check() {
  clock_enabled || return 0
  [ -x "$PROBE" ] || die "нет $PROBE -- make -j$(nproc)   (или CLOCK=0)"
  systemctl stop chrony 2>/dev/null || true
}

# Отражатель (rx). Выходит сам через PROBE_IDLE_S тишины, поэтому вызов
# блокирующий и ровно на время замера -- во время самого прогона он не крутится.
# Интерфейс, через который уходит маршрут по умолчанию, и его адрес. Ошибки
# гасятся в "", а не остаются кодом возврата: под `set -e` с `pipefail` упавший
# `ip` убил бы скрипт прямо на присваивании, не дав напечатать причину -- ровно
# так молча умирал rx.sh, когда MGMT_IFACE был зашит в несуществующий ens5.
mgmt_iface() {
  if [ -n "$MGMT_IFACE" ]; then echo "$MGMT_IFACE"; return 0; fi
  ip -4 route show default 2>/dev/null | awk '/dev/ {for (i=1;i<NF;i++) if ($i=="dev") {print $(i+1); exit}}' || true
}

mgmt_ip() {
  local iface=$1
  [ -n "$iface" ] || return 0
  ip -4 -o addr show "$iface" 2>/dev/null | awk '{print $4}' | cut -d/ -f1 || true
}

clock_reflect() {
  clock_enabled || return 0
  local iface ip
  iface=$(mgmt_iface)
  ip=$(mgmt_ip "$iface")
  [ -n "$ip" ] || die "не определить адрес управляющего интерфейса${iface:+ ($iface)}.
   Посмотреть: ip -4 -o addr show; задать явно MGMT_IFACE=<имя>, либо CLOCK=0."
  row "отражатель $1" "$ip:$PROBE_PORT, ждёт до ${PROBE_WAIT_S}с, выход через ${PROBE_IDLE_S}с тишины"
  "$PROBE" --reflect --bind "$ip" --port "$PROBE_PORT" \
    --idle-s "$PROBE_IDLE_S" --wait-s "$PROBE_WAIT_S" \
    >/dev/null 2>&1 || true
}

# Пробник (tx). Печатает смещение в нс на stdout; "p50=" и число разделены
# пробелами, поэтому по полям awk их не собрать. Минус обязателен.
clock_probe_once() {
  "$PROBE" --probe --peer "$2:$PROBE_PORT" --count "$PROBE_COUNT" 2>&1 \
    | tee -a "$1" \
    | sed -nE 's/.*clock offset[[:space:]]+p50=[[:space:]]*(-?[0-9]+).*/\1/p'
}

# Обойти всех приёмников по очереди. Печатает по строке "адрес смещение момент" на
# каждого; пустое смещение = замер не удался, решает вызывающий. Последовательно, а
# не параллельно: пинг-понг мерит путь, и четыре одновременных пробника на одном
# ядре мерили бы уже друг друга.
#
# Третье поле -- СЕРЕДИНА замера в наносекундах эпохи, по тем же часам, что
# send_ts_ns в отсчётах (CLOCK_REALTIME на этой машине). Без него поправку не к
# чему привязать: приёмников обходим по очереди, и четвёртый меряется на полтора
# десятка секунд позже первого. Натягивать его смещение на тот же отрезок, что и
# первому, значит завышать наклон -- и тем сильнее, чем быстрее уходят часы.
clock_probe_all() {
  local log=$1; shift
  local peer off t0 t1
  for peer in "$@"; do
    t0=$(date +%s%N)
    off=$(clock_probe_once "$log" "$peer")
    t1=$(date +%s%N)
    echo "$peer ${off:-} $(( (t0 + t1) / 2 ))"
  done
}
