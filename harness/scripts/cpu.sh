#!/usr/bin/env bash
# Ядра: погасить SMT-сиблинги, зафиксировать частоту.
#
#   ./scripts/cpu.sh              что онлайн, governor, изоляция
#   sudo ./scripts/cpu.sh apply   погасить OFFLINE_CPUS, governor=GOVERNOR
#   sudo ./scripts/cpu.sh revert  включить всё обратно, вернуть governor
#
# Гипертред делит с соседом исполнительные блоки и L1/L2. Пока busy-poll крутит
# пустой цикл, сосед по физическому ядру отъедает у него слоты выдачи -- поэтому
# сиблинг изолированного ядра гасится, а не просто оставляется свободным.
#
# isolcpus/nohz_full здесь НЕ применяются: это параметры GRUB, нужен reboot.
# Скрипт только сверяет, что в /proc/cmdline написано то же, что в host.conf.
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/lib.sh"
SUBCOMMANDS="status apply revert"

governor_of() { cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "-"; }
no_turbo_of() { cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo "-"; }

cmd_status() {
  say "Топология"
  lscpu -e=CPU,CORE,SOCKET,ONLINE 2>/dev/null | sed 's/^/   /'
  echo
  row "online" "$(cat /sys/devices/system/cpu/online)"
  row "offline" "$(cat /sys/devices/system/cpu/offline 2>/dev/null || echo нет)"

  say "Частота"
  row "governor" "$(governor_of)"
  row "no_turbo" "$(no_turbo_of)"
  [ -f "$STATE_FILE" ] && row "сохранено для revert" "$(tr '\n' ' ' < "$STATE_FILE")"

  say "Изоляция (из /proc/cmdline, меняется только через GRUB + reboot)"
  local cmdline; cmdline=$(cat /proc/cmdline)
  for p in isolcpus nohz_full rcu_nocbs idle; do
    local v; v=$(tr ' ' '\n' <<<"$cmdline" | grep "^$p=" || true)
    if [ -n "$v" ]; then ok "$v"; else bad "$p= отсутствует"; fi
  done

  if [ -n "$ISOL_CPUS" ]; then
    local got; got=$(tr ' ' '\n' <<<"$cmdline" | sed -n 's/^isolcpus=//p')
    [ "$got" = "$ISOL_CPUS" ] || warn "isolcpus=$got, а в host.conf ISOL_CPUS=$ISOL_CPUS"
  fi

  say "Проверки"
  local off; off=$(expand_cpus "$OFFLINE_CPUS")
  for c in $off; do
    if [ "$(cat "/sys/devices/system/cpu/cpu$c/online" 2>/dev/null)" = 0 ]
      then ok "cpu$c выключен"
      else warn "cpu$c должен быть выключен -- sudo ./scripts/cpu.sh apply"
    fi
  done
  # Ядро под busy-poll не должно делить физическое ядро ни с кем живым.
  check_sibling "$CPU_RECEIVER" приёмник
  check_sibling "$CPU_SENDER" отправитель
}

# Печатает, с кем делит физическое ядро указанный CPU.
check_sibling() {
  local cpu=$1 role=$2 core mates=""
  core=$(lscpu --parse=CPU,CORE 2>/dev/null | grep -v '^#' | awk -F, -v c="$cpu" '$1==c {print $2}')
  [ -n "$core" ] || return 0
  mates=$(lscpu --parse=CPU,CORE 2>/dev/null | grep -v '^#' \
          | awk -F, -v k="$core" -v c="$cpu" '$2==k && $1!=c {printf "%s ", $1}')
  if [ -z "$mates" ]; then
    ok "cpu$cpu ($role) один на физическом ядре $core"
  else
    warn "cpu$cpu ($role) делит ядро $core с cpu${mates% } -- погасите сиблинг"
  fi
}

cmd_apply() {
  need_root apply
  # Состояние пишем один раз: повторный apply не должен затереть исходный
  # governor уже применённым значением.
  if [ ! -f "$STATE_FILE" ]; then
    { echo "governor $(governor_of)"; echo "no_turbo $(no_turbo_of)"; } > "$STATE_FILE"
  fi

  local off; off=$(expand_cpus "$OFFLINE_CPUS")
  if [ -n "$off" ]; then
    say "Выключаю ядра: $OFFLINE_CPUS"
    for c in $off; do
      [ -w "/sys/devices/system/cpu/cpu$c/online" ] || { warn "cpu$c не выключается"; continue; }
      echo 0 > "/sys/devices/system/cpu/cpu$c/online"
    done
    row "online" "$(cat /sys/devices/system/cpu/online)"
  fi

  if [ -n "$GOVERNOR" ]; then
    say "Governor: $GOVERNOR"
    local n=0
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
      [ -w "$g" ] || continue
      echo "$GOVERNOR" > "$g" && n=$((n + 1))
    done
    # На AWS cpufreq обычно не отдан гостю: частотой правит гипервизор.
    [ "$n" -gt 0 ] && ok "применён на $n ядрах" || warn "cpufreq недоступен (нормально для EC2)"
  fi

  if [ -n "$NO_TURBO" ]; then
    say "no_turbo: $NO_TURBO"
    if [ -w /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
      echo "$NO_TURBO" > /sys/devices/system/cpu/intel_pstate/no_turbo && ok применён
    else
      warn "intel_pstate недоступен (нормально для EC2)"
    fi
  fi
}

cmd_revert() {
  need_root revert
  say "Включаю все ядра"
  for f in /sys/devices/system/cpu/cpu*/online; do
    [ -w "$f" ] && echo 1 > "$f" 2>/dev/null || true
  done
  row "online" "$(cat /sys/devices/system/cpu/online)"

  if [ -f "$STATE_FILE" ]; then
    local gov nt
    gov=$(awk '/^governor/ {print $2}' "$STATE_FILE")
    nt=$(awk '/^no_turbo/ {print $2}' "$STATE_FILE")
    say "Возврат частоты: governor=$gov no_turbo=$nt"
    if [ "$gov" != "-" ]; then
      for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [ -w "$g" ] && echo "$gov" > "$g" 2>/dev/null || true
      done
    fi
    [ "$nt" != "-" ] && echo "$nt" > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || true
    rm -f "$STATE_FILE"
    ok "восстановлено"
  else
    warn "нечего восстанавливать (нет $STATE_FILE)"
  fi
}

dispatch "${1:-status}"
