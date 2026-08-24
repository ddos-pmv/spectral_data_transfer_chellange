#!/usr/bin/env bash
# То, что скриптом не чинится: параметры загрузки, сборка, часы.
#
#   ./scripts/checkenv.sh
#
# Состояние железа (ядра, память, прерывания, карта) показывает host.sh status.
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/lib.sh"
cd "$ROOT_DIR"

FAIL=0
note() { bad "$*"; FAIL=$((FAIL + 1)); }

# ------------------------------------------------------------------- GRUB
say "Параметры загрузки"
CMDLINE=$(cat /proc/cmdline)
row "/proc/cmdline" "$CMDLINE"
echo
want_param() {
  local key=$1 why=$2 got
  got=$(tr ' ' '\n' <<<"$CMDLINE" | grep "^$key=" || true)
  if [ -n "$got" ]; then ok "$got"; else note "$key= нет -- $why"; fi
}
want_param isolcpus   "планировщик будет ставить чужие задачи на рабочее ядро"
want_param nohz_full  "тик таймера 250/1000 раз в секунду попадёт в p99.9"
want_param rcu_nocbs  "RCU-колбэки будут выполняться на рабочем ядре"
want_param idle       "нужен idle=poll: выход из C1E стоит ~10 мкс, из C6 ~130"
want_param transparent_hugepage "khugepaged берёт mmap_lock и даёт выбросы в хвосте"

# Ubuntu cloud-образ кладёт в /etc/default/grub.d/50-cloudimg-settings.cfg
# присваивание GRUB_CMDLINE_LINUX_DEFAULT=..., которое затирает то, что написано
# в /etc/default/grub. Признак: в cmdline нет даже quiet/splash.
if ! grep -q 'idle=' <<<"$CMDLINE" && [ -f /etc/default/grub.d/50-cloudimg-settings.cfg ]; then
  warn "правки в /etc/default/grub мог затереть grub.d/50-cloudimg-settings.cfg"
  echo "     положите свои параметры в /etc/default/grub.d/99-hft.cfg и допишите"
  echo "     \$GRUB_CMDLINE_LINUX_DEFAULT в конец, иначе они не доедут"
fi
echo "   ядро молча игнорирует неизвестные параметры: опечатка выглядит как"
echo "   рабочая строка. Перед reboot сверяйте: grep hft /boot/grub/grub.cfg"

# ------------------------------------------------------------------- DPDK
say "DPDK"
if pkg-config --exists libdpdk 2>/dev/null; then
  ok "libdpdk $(pkg-config --modversion libdpdk)"
else
  note "libdpdk не найден -- sudo ./scripts/build.sh"
fi
PMDS=$(find /usr/local/lib/x86_64-linux-gnu -maxdepth 2 -type d -name 'pmds-*' 2>/dev/null | head -1)
if [ -n "$PMDS" ]; then
  for d in net_ena net_af_xdp; do
    [ -e "$PMDS/librte_$d.so" ] && ok "$d" || warn "$d не собран"
  done
else
  note "каталог pmds-* не найден -- EAL не увидит драйверы"
fi
lsmod 2>/dev/null | grep -q '^igb_uio' && ok "igb_uio загружен" \
  || warn "igb_uio не загружен -- нужен для ENA PMD с Write Combining"

# ------------------------------------------------------------------ сборка
say "Бинари"
for b in producer consumer dpdk_sender dpdk_reciever; do
  if [ -x "bin/$b" ]; then
    ok "bin/$b"
  else
    note "bin/$b нет -- make -j\$(nproc)"
  fi
done

# ------------------------------------------------------------------- часы
say "Часы"
# Замеры считаются как recv_ts(машина B) - send_ts(машина A) по CLOCK_REALTIME,
# поэтому расхождение часов входит в результат один в один. При 4 мкс медианы
# смещение в 6 мкс делает измерение бессмысленным.
if command -v chronyc >/dev/null 2>&1 && chronyc tracking >/dev/null 2>&1; then
  chronyc tracking | grep -E 'Reference ID|System time|Frequency|Root dispersion|Update interval' | sed 's/^/   /'
  echo
  OFF=$(chronyc tracking | awk '/System time/ {print $4}')
  row "смещение, с" "$OFF"
  warn "chrony непрерывно подкручивает частоту, поэтому смещение не константа."
  echo "     Перед серией замеров остановить его на ОБЕИХ машинах:"
  echo "       sudo systemctl stop chrony"
  echo "     и прогонять в обе стороны: d = (AB+BA)/2, смещение = (AB-BA)/2"
else
  warn "chronyd не отвечает (остановлен для замеров?)"
fi
if ls /dev/ptp* >/dev/null 2>&1; then
  ok "PHC есть: $(ls /dev/ptp*)"
else
  row "PHC" "нет -- ENA на этих инстансах его не отдаёт, только Amazon Time Sync"
fi

say "Итого"
[ "$FAIL" = 0 ] && ok "всё на месте" || bad "проблем: $FAIL"
echo "   состояние железа: ./scripts/host.sh status"
exit 0
