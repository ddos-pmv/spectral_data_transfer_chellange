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
eal_dev() {
  [ -z "${EAL_DEV:-}" ] || { echo "$EAL_DEV"; return; }
  local pci; pci=$(resolve_pci 2>/dev/null) || pci=""
  if [ -n "$pci" ] && [ "$(pci_driver "$pci")" = "$DPDK_DRIVER" ]; then
    echo "-a $pci"
  else
    echo "--vdev net_af_xdp0,iface=$DPDK_IFACE,start_queue=0,queue_count=1"
  fi
}

# В режиме AF_XDP карта видна ядру, адреса берутся через ip; в режиме PMD -- нет.
kernel_mode() { case "$(eal_dev)" in *net_af_xdp*) return 0 ;; *) return 1 ;; esac; }

# IP той карты, через которую реально идёт трафик. VPC режет пакеты, у которых
# src IP не совпадает с адресом ENI, так что брать адрес с соседней нельзя.
local_ip() {
  if kernel_mode; then
    ip -4 -o addr show "$DPDK_IFACE" 2>/dev/null | awk '{print $4}' | cut -d/ -f1
  else
    eni_addrs "$DPDK_ENI_INDEX" 2>/dev/null | awk '{print $2}'
  fi
}
