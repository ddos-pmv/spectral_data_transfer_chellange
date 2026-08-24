#!/usr/bin/env bash
# Карта под DPDK: привязка к igb_uio и обратно к ядру.
#
#   ./scripts/nic.sh            состояние: PCI, драйвер, MAC, IP, Write Combining
#   sudo ./scripts/nic.sh bind      отдать карту DPDK
#   sudo ./scripts/nic.sh unbind    вернуть карту ядру
#
# Привязка живёт в памяти ядра и слетает при перезагрузке -- это страховка:
# если что-то сломается, машина чинится обычным reboot.
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/lib.sh"
SUBCOMMANDS="status bind unbind"

cmd_status() {
  say "Карта DPDK"
  local pci drv
  if ! pci=$(resolve_pci); then
    bad "PCI не определить: интерфейса $DPDK_IFACE нет и DPDK_PCI пуст"
    echo "   впишите DPDK_PCI в scripts/host.conf (см. dpdk-devbind.py --status)"
    return 0
  fi
  drv=$(pci_driver "$pci")
  row "PCI" "$pci"
  row "интерфейс" "$DPDK_IFACE"
  row "драйвер" "$drv"

  case $drv in
    "$DPDK_DRIVER") ok "карта у DPDK" ;;
    "$KERNEL_DRIVER") warn "карта у ядра -- для прогона нужен bind" ;;
    *) warn "неожиданный драйвер '$drv'" ;;
  esac

  # ens5 не трогаем никогда: на нём SSH. Проверяем, что не перепутали.
  local ssh_pci
  ssh_pci=$(basename "$(readlink -f /sys/class/net/ens5/device)" 2>/dev/null || echo "")
  [ "$pci" = "$ssh_pci" ] && bad "это PCI интерфейса ens5 -- отвяжете и потеряете SSH"

  say "Адреса (подставить на второй машине в PEER_MAC/PEER_IP)"
  local addrs mac ip
  if addrs=$(eni_addrs "$DPDK_ENI_INDEX"); then
    read -r mac ip <<<"$addrs"
    row "PEER_MAC" "$mac"
    row "PEER_IP" "$ip"
  else
    bad "IMDS не ответил -- ENI $DPDK_ENI_INDEX не прочитать"
  fi

  say "Write Combining"
  wc_status "$pci"

  say "EAL"
  row "аргумент" "-a $pci"
}

# Настоящая проверка WC -- только таблица PAT, и только пока приложение держит
# устройство открытым. Через sysfs никак: wc_activate объявлен с правами 0,
# файла /sys/module/igb_uio/parameters/wc_activate не существует в принципе.
wc_status() {
  local pci=$1 bar
  bar=$(lspci -v -s "$pci" 2>/dev/null \
        | awk '/Memory at/ && /prefetchable/ {print $3; exit}')
  [ -n "$bar" ] || { warn "prefetchable BAR не найден (lspci от root?)"; return 0; }
  row "prefetchable BAR" "$bar"

  if [ ! -r /sys/kernel/debug/x86/pat_memtype_list ]; then
    warn "нужен root, чтобы прочитать pat_memtype_list"
    return 0
  fi
  # В pat_memtype_list адрес напечатан как 0x0000000088000000, в lspci -- как
  # 88000000, поэтому ищем вхождением, а не точным совпадением.
  local wc
  wc=$(grep -i write-combining /sys/kernel/debug/x86/pat_memtype_list 2>/dev/null || true)
  if printf '%s' "$wc" | grep -qi "$bar"; then
    ok "регион отображён как write-combining"
  elif [ -n "$wc" ]; then
    warn "write-combining в таблице есть, но не на этом BAR -- проверьте руками"
    printf '%s\n' "$wc" | sed 's/^/     /'
  else
    warn "WC-регионов нет. Если приложение сейчас не запущено -- это нормально,"
    echo "     проверять надо во время работы tx.sh. Если запущено -- LLQ идёт"
    echo "     через UC, каждая запись дескриптора отдельной PCIe-транзакцией."
  fi
}

cmd_bind() {
  need_root bind
  local pci drv
  pci=$(resolve_pci) || die "PCI не определить: впишите DPDK_PCI в host.conf"
  drv=$(pci_driver "$pci")
  [ "$drv" = "$DPDK_DRIVER" ] && { ok "уже привязана к $DPDK_DRIVER"; return 0; }

  say "Привязка $pci к $DPDK_DRIVER"
  if [ "$DPDK_DRIVER" = igb_uio ]; then
    modprobe uio
    modprobe igb_uio || die "igb_uio не собран -- сначала ./scripts/build.sh"
  else
    modprobe "$DPDK_DRIVER"
  fi
  ip link set "$DPDK_IFACE" down 2>/dev/null || true
  dpdk-devbind.py --bind="$DPDK_DRIVER" "$pci"
  ok "$pci -> $DPDK_DRIVER"
}

cmd_unbind() {
  need_root unbind
  local pci
  pci=$(resolve_pci) || die "PCI не определить: впишите DPDK_PCI в host.conf"
  [ "$(pci_driver "$pci")" = "$KERNEL_DRIVER" ] && { ok "уже у ядра"; return 0; }

  say "Возврат $pci ядру ($KERNEL_DRIVER)"
  dpdk-devbind.py --bind="$KERNEL_DRIVER" "$pci"
  # Имя интерфейса и адрес возвращает netplan; без него карта поднимется без IP.
  netplan apply 2>/dev/null || true
  ip link set "$DPDK_IFACE" up 2>/dev/null || true
  ip -4 -o addr show "$DPDK_IFACE" 2>/dev/null | sed 's/^/   /'
  ok "$pci -> $KERNEL_DRIVER"
}

dispatch "${1:-status}"
