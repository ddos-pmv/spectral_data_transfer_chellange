#!/usr/bin/env bash
# Проверка окружения для DPDK-прогона. НИЧЕГО НЕ МЕНЯЕТ:
# только смотрит и печатает команды, которые нужно выполнить самому.
#
#   ./scripts/checkenv.sh
#   IFACE=ens5 ./scripts/checkenv.sh
set -uo pipefail

IFACE=${IFACE:-ens5}

WARNS=0
sec()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
ok()   { printf '  \033[32mok  \033[0m %s\n' "$*"; }
warn() { printf '  \033[33mWARN\033[0m %s\n' "$*"; WARNS=$((WARNS+1)); }
hint() { printf '       %s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

read -r CMDLINE < /proc/cmdline
ISOLATED=$(cat /sys/devices/system/cpu/isolated 2>/dev/null)
NPROC=$(nproc)

# Развернуть "2,4-6" в "2 4 5 6".
expand() {
  local part lo hi
  for part in ${1//,/ }; do
    lo=${part%-*}; hi=${part#*-}
    for ((i=lo; i<=hi; i++)); do echo "$i"; done
  done
}

# ---------------------------------------------------------------- машина
sec "Машина"
ok "ядер: $NPROC"
if have systemd-detect-virt && [ "$(systemd-detect-virt)" != none ]; then
  ok "виртуализация: $(systemd-detect-virt)"
fi
if have curl; then
  INST=$(curl -s --max-time 1 \
    http://169.254.169.254/latest/meta-data/instance-type 2>/dev/null)
  case "${INST:-}" in
    "") ;;
    t2.*|t3.*|t4g.*)
      ok "инстанс AWS: $INST"
      warn "burstable-инстанс: CPU-кредиты и шумные соседи"
      hint "числа по хвосту отсюда брать нельзя — только проверка работоспособности" ;;
    *)  ok "инстанс AWS: $INST" ;;
  esac
fi
if [ "$NPROC" -le 2 ]; then
  warn "двух ядер не хватает на изоляцию (нужно ядро под ОС + по ядру на процесс)"
  hint "эта машина годится для функциональной проверки, не для замеров"
fi
STEAL=$(awk '/^cpu /{print $9}' /proc/stat)
[ "${STEAL:-0}" -gt 0 ] && warn "steal time не ноль ($STEAL тиков) — гипервизор отбирает такты"

# ----------------------------------------------------------------- ядро
sec "Параметры ядра"
for p in isolcpus nohz_full rcu_nocbs irqaffinity; do
  V=$(grep -o "\b$p=[^ ]*" <<<"$CMDLINE")
  if [ -n "$V" ]; then ok "$V"; else warn "$p не задан"; fi
done
if [ -z "$ISOLATED" ]; then
  hint "добавить в /etc/default/grub, GRUB_CMDLINE_LINUX_DEFAULT, затем update-grub и reboot:"
  hint "  isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 irqaffinity=0,1"
  hint "  isolcpus убирает задачи, irqaffinity — прерывания. Это РАЗНЫЕ вещи."
fi

# ------------------------------------------------------------- топология
sec "Топология ядер"
if [ -n "$ISOLATED" ]; then
  ok "изолированы: $ISOLATED"
  for c in $(expand "$ISOLATED"); do
    SIB=$(cat "/sys/devices/system/cpu/cpu$c/topology/thread_siblings_list" 2>/dev/null)
    for s in $(expand "$SIB"); do
      [ "$s" = "$c" ] && continue
      if ! grep -qw "$s" <<<"$(expand "$ISOLATED" | tr '\n' ' ')"; then
        warn "cpu$c изолирован, а его HT-сосед cpu$s — нет (делят L1/L2 и порты)"
        hint "изолировать пару целиком или занимать только по одному ядру из пары"
      fi
    done
  done
else
  warn "изолированных ядер нет — busy-spin процессы будут вытесняться планировщиком"
fi

# -------------------------------------------------------------- частоты
sec "Частоты"
GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)
if [ -z "$GOV" ]; then
  ok "cpufreq недоступен (обычно так в облаке — частотой управляет гипервизор)"
elif [ "$GOV" = performance ]; then
  ok "governor: performance"
else
  warn "governor: $GOV"
  hint "  sudo cpupower frequency-set -g performance"
fi

# --------------------------------------------------------------- память
sec "Память"
HP_TOTAL=$(awk '/HugePages_Total/{print $2}' /proc/meminfo)
HP_FREE=$(awk '/HugePages_Free/{print $2}' /proc/meminfo)
if [ "${HP_TOTAL:-0}" -gt 0 ]; then
  ok "hugepages 2M: всего $HP_TOTAL, свободно $HP_FREE"
  [ "$HP_FREE" -lt 512 ] && warn "мало свободных — возможно, висит недобитый процесс DPDK"
else
  warn "hugepages не выделены"
  hint "  sudo sh -c 'echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'"
fi
THP=$(sed -n 's/.*\[\(.*\)\].*/\1/p' /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null)
[ "$THP" = always ] && warn "THP=always — фоновая дефрагментация даёт выбросы в хвост" \
  && hint "  sudo sh -c 'echo madvise > /sys/kernel/mm/transparent_hugepage/enabled'"
ML=$(ulimit -l)
if [ "$ML" = unlimited ] || [ "${ML:-0}" -ge 65536 ]; then ok "memlock: $ML"
else warn "memlock $ML КБ маловато для umem AF_XDP (ulimit -l в сессии запуска)"; fi

# ----------------------------------------------------------- прерывания
sec "Прерывания"
if pgrep -x irqbalance >/dev/null; then
  warn "irqbalance работает и будет переносить прерывания обратно"
  hint "  sudo systemctl stop irqbalance && sudo systemctl disable irqbalance"
else
  ok "irqbalance не запущен"
fi
if [ -n "$ISOLATED" ]; then
  ISO=$(expand "$ISOLATED" | tr '\n' ' ')
  FOUND=0
  # Не счётчики из /proc/interrupts (они накопленные с загрузки и мало о чём
  # говорят), а то, КУДА прерыванию вообще разрешено приходить.
  for d in /proc/irq/[0-9]*; do
    AFF=$(cat "$d/smp_affinity_list" 2>/dev/null) || continue
    ALLOWED=" $(expand "$AFF" | tr '\n' ' ') "
    for c in $ISO; do
      case "$ALLOWED" in
        *" $c "*)
          NAME=$(ls "$d" 2>/dev/null | grep -vE '^(smp_affinity|smp_affinity_list|node|spurious|affinity_hint|effective_affinity|effective_affinity_list)$' | head -1)
          warn "IRQ ${d##*/} (${NAME:-?}) разрешён на cpu$c (affinity: $AFF)"
          FOUND=1; break ;;
      esac
    done
  done
  if [ "$FOUND" = 0 ]; then
    ok "ни одно device-прерывание не нацелено на изолированные ядра"
  else
    hint "подвижные переносятся так:  echo 0-1 | sudo tee /proc/irq/<N>/smp_affinity_list"
    hint "managed MSI-X (NVMe, ENA, Wi-Fi) так не переносятся — только irqaffinity= в GRUB"
  fi
  TRM=$(awk '$1=="TRM:"{s=0; for(i=2;i<=NF;i++) if($i ~ /^[0-9]+$/) s+=$i; print s}' /proc/interrupts)
  [ "${TRM:-0}" -gt 0 ] && warn "термальных прерываний (TRM): $TRM — убрать нельзя, это пол по хвосту"
  hint "динамика за интервал, а не с загрузки:  sudo ./scripts/irq.py 30 $ISOLATED"
fi

# ------------------------------------------------------------------ сеть
sec "Сеть: $IFACE"
if [ ! -e "/sys/class/net/$IFACE" ]; then
  warn "интерфейса $IFACE нет"
  hint "есть: $(ls /sys/class/net | tr '\n' ' ')"
  hint "задать другой: IFACE=eth0 $0"
else
  ok "состояние: $(cat /sys/class/net/$IFACE/operstate)"
  MTU=$(cat "/sys/class/net/$IFACE/mtu")
  # XDP требует, чтобы кадр влезал в страницу 4 КБ за вычетом headroom и
  # служебных структур; у ENA предел ~3498. При jumbo (в VPC по умолчанию 9001)
  # XDP-программа просто не навешивается.
  if [ "$MTU" -gt 3498 ]; then
    warn "MTU $MTU — XDP не навесится (предел около 3498)"
    hint "  sudo ip link set $IFACE mtu 1500     # наши кадры не больше 258 байт"
  else
    ok "MTU: $MTU"
  fi
  ok "MAC: $(cat /sys/class/net/$IFACE/address)"
  IP=$(ip -4 -o addr show "$IFACE" 2>/dev/null | awk '{print $4}')
  [ -n "$IP" ] && ok "IPv4: $IP" || warn "нет IPv4 — src-ip для отправителя взять неоткуда"
  if have ethtool; then
    DRV=$(ethtool -i "$IFACE" 2>/dev/null | awk '/^driver:/{print $2}')
    [ -n "$DRV" ] && ok "драйвер: $DRV" || warn "ethtool -i не отвечает — интерфейс поднят?"
    COMBINED=$(ethtool -l "$IFACE" 2>/dev/null | awk '/^Combined:/{c=$2} END{print c}')
    if [ "${COMBINED:-1}" -gt 1 ]; then
      warn "очередей $COMBINED, а AF_XDP слушает только queue 0 — RSS раскидает поток мимо"
      hint "  sudo ethtool -L $IFACE combined 1"
    else
      ok "очередей: 1 (весь поток попадёт в queue 0)"
    fi
    KOUT=$(ethtool -k "$IFACE" 2>/dev/null)
    OFFL=$(printf '%s\n' "$KOUT" | grep -E '^(generic-receive-offload|generic-segmentation-offload|tcp-segmentation-offload): on' | cut -d: -f1 | tr '\n' ' ')
    if [ -z "$KOUT" ]; then
      warn "ethtool -k не отвечает (интерфейс поднят?) — оффлоады не проверены"
    elif [ -n "$OFFL" ]; then
      warn "включены оффлоады: $OFFL — могут склеить или нарезать наши кадры"
      hint "  sudo ethtool -K $IFACE gro off gso off tso off"
    else
      ok "gro/gso/tso выключены"
    fi
  else
    warn "нет ethtool"; hint "  sudo apt install -y ethtool"
  fi
fi

# ------------------------------------------------------------------ DPDK
sec "DPDK"
if have pkg-config && pkg-config --exists libdpdk; then
  ok "libdpdk $(pkg-config --modversion libdpdk)"
  PMDS=$(find "$(pkg-config --variable=libdir libdpdk)" -path '*pmds*' -name 'librte_net_af_xdp.so*' 2>/dev/null | head -1)
  if [ -n "$PMDS" ]; then ok "драйвер af_xdp: $PMDS"
  else warn "librte_net_af_xdp.so не найден — PMD подгружается в рантайме, без него vdev не поднимется"; fi
  # Без пайпа в grep -q: при set -o pipefail ранний выход grep роняет ldconfig по SIGPIPE.
  LDCACHE=$(ldconfig -p 2>/dev/null)
  case "$LDCACHE" in
    *librte_eal*) ok "librte_* в кэше ldconfig (PMD подгружаются в рантайме)" ;;
    *) warn "librte_* не в кэше ldconfig — EAL не найдёт драйверы"
       hint "  echo $(pkg-config --variable=libdir libdpdk) | sudo tee /etc/ld.so.conf.d/dpdk.conf && sudo ldconfig" ;;
  esac
else
  warn "pkg-config не видит libdpdk — dpdk_* цели не соберутся"
fi
[ -x ../bin/dpdk_sender ] && ok "bin/dpdk_sender собран" || warn "bin/dpdk_sender нет: make -j\$(nproc)"

printf '\n'
[ "$WARNS" = 0 ] && printf '\033[32mвсё чисто\033[0m\n' || printf '\033[33mпредупреждений: %s\033[0m\n' "$WARNS"
