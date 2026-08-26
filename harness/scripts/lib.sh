# Общая часть всех host-скриптов. Подключается через `.`, сама ничего не делает.
#
# Даёт: чтение host.conf, значения по умолчанию, вывод, root-проверку,
# разрешение PCI-адреса и чтение адресов ENI из метаданных инстанса.

# Каталог скриптов, независимо от того, откуда позвали.
LIB_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(dirname "$LIB_DIR")

if [ -f "$LIB_DIR/host.conf" ]; then
  # shellcheck disable=SC1091
  . "$LIB_DIR/host.conf"
elif [ -z "${HOSTCONF_WARNED:-}" ]; then
  # host.sh зовёт четыре скрипта подряд, каждый подключает lib.sh -- ругаемся один раз.
  export HOSTCONF_WARNED=1
  echo "нет scripts/host.conf -- беру значения по умолчанию" >&2
  echo "  cp scripts/host.conf.example scripts/host.conf" >&2
fi

# Значения по умолчанию ставим ПОСЛЕ конфига и через :-, поэтому переменная
# окружения перебивает файл, а файл перебивает эти дефолты.
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
check_pool() {  # check_pool ИМЯ_ПУЛА ПУЛ ИМЯ_ОЧЕРЕДИ ДЕСКРИПТОРЫ BURST
  local need=$(( $4 + $5 * 2 + 256 ))
  [ "$2" -gt "$need" ] || die "$1=$2 мало при $3=$4 и BURST=$5: нужно больше $need
   (очередь $4 + burst в полёте + кэш mempool 256)
   возьмите $1=$(( $4 * 2 + 4095 ))"
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
# Аргумент устройства для EAL. Если карта уже у DPDK -- адресуем её по PCI,
# иначе поднимаем AF_XDP поверх живого интерфейса (ядро видит карту, SSH цел).
# Молчаливой подмены режима тут быть не должно: AF_XDP на ENA не поднимается
# в принципе, и подстановка его вместо PCI даёт непонятную ошибку вместо
# внятного "карта не привязана". Для локального стенда на veth EAL_DEV
# задаётся явно.
eal_dev() {
  [ -z "${EAL_DEV:-}" ] || { echo "$EAL_DEV"; return 0; }
  local pci; pci=$(resolve_pci 2>/dev/null) || pci=""
  [ -n "$pci" ] || return 1
  [ "$(pci_driver "$pci")" = "$DPDK_DRIVER" ] || return 1
  echo "-a $pci"
}

# В режиме AF_XDP карта видна ядру, адреса берутся через ip; в режиме PMD -- нет.
kernel_mode() { case "${EAL_DEV:-}" in *net_af_xdp*) return 0 ;; *) return 1 ;; esac; }

# IP той карты, через которую реально идёт трафик. VPC режет пакеты, у которых
# src IP не совпадает с адресом ENI, так что брать адрес с соседней нельзя.
local_ip() {
  if kernel_mode; then
    ip -4 -o addr show "$DPDK_IFACE" 2>/dev/null | awk '{print $4}' | cut -d/ -f1
  else
    eni_addrs "$DPDK_ENI_INDEX" 2>/dev/null | awk '{print $2}'
  fi
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
