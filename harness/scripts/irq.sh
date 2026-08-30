#!/usr/bin/env bash
# Прерывания: согнать все на housekeeping-ядро и вернуть обратно.
#
#   ./scripts/irq.sh              кто где сидит
#   sudo ./scripts/irq.sh apply   все IRQ на IRQ_CPUS, irqbalance выключить
#   sudo ./scripts/irq.sh revert  вернуть на все ядра, irqbalance включить
#
# Прерывание, прилетевшее на изолированное ядро, выбивает busy-poll из цикла
# на единицы микросекунд -- это прямо видно в p99.9. isolcpus от прерываний НЕ
# защищает: он убирает ядро из планировщика задач, а не из масок IRQ.
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/lib.sh"
SUBCOMMANDS="status apply revert"

# Имя устройства из /proc/interrupts по номеру IRQ.
irq_device() {
  awk -v irq="^ *$1:" '$1 ~ irq {
    for (i = NF; i > 0; i--) if ($i ~ /[A-Za-z]/) { print $i; exit }
  }' /proc/interrupts
}

cmd_status() {
  say "Прерывания (цель: $IRQ_CPUS)"
  printf '   %s%s%s%s\n' "$(pad IRQ 7)" "$(pad ядра 13)" "$(pad счётчик 10)" устройство
  local irq cores dev count stray=0
  for d in /proc/irq/[0-9]*; do
    irq=$(basename "$d")
    [ -f "$d/smp_affinity_list" ] || continue
    cores=$(cat "$d/smp_affinity_list")
    dev=$(irq_device "$irq"); [ -n "$dev" ] || dev="[не используется]"
    count=$(awk -v irq="^ *$irq:" '$1 ~ irq {s=0; for(i=2;i<=NF;i++) if($i ~ /^[0-9]+$/) s+=$i; print s; exit}' /proc/interrupts)
    printf '   %-6s %-12s %-9s %s\n' "$irq" "$cores" "${count:-0}" "$dev"
    [ "$cores" = "$IRQ_CPUS" ] || stray=$((stray + 1))
  done

  say "Итого"
  if [ "$stray" = 0 ]; then ok "все IRQ на $IRQ_CPUS"
  else warn "$stray IRQ не на $IRQ_CPUS -- sudo ./scripts/irq.sh apply"; fi
  if systemctl is-active --quiet irqbalance 2>/dev/null; then
    bad "irqbalance работает и разгонит привязку обратно"
  else
    ok "irqbalance не работает"
  fi
}

cmd_apply() {
  need_root apply
  say "irqbalance"
  if systemctl is-enabled irqbalance >/dev/null 2>&1 || systemctl is-active --quiet irqbalance; then
    systemctl disable --now irqbalance 2>/dev/null || systemctl stop irqbalance
    ok "выключен"
  else
    ok "уже выключен"
  fi

  say "Перенос IRQ на $IRQ_CPUS"
  local irq moved=0 locked=0
  for d in /proc/irq/[0-9]*; do
    irq=$(basename "$d")
    [ -w "$d/smp_affinity_list" ] || continue
    # Часть IRQ (таймеры, IPI) ядро привязывает жёстко и запись отвергает.
    # Это нормально, поэтому просто считаем их, а не падаем.
    if echo "$IRQ_CPUS" > "$d/smp_affinity_list" 2>/dev/null; then
      moved=$((moved + 1))
    else
      locked=$((locked + 1))
    fi
  done
  ok "перенесено $moved"
  [ "$locked" = 0 ] || row "заблокировано ядром" "$locked (нормально: таймеры и IPI)"
}

cmd_revert() {
  need_root revert
  local all
  all="0-$(($(nproc --all) - 1))"
  say "Возврат IRQ на все ядра ($all)"
  for d in /proc/irq/[0-9]*; do
    [ -w "$d/smp_affinity_list" ] || continue
    echo "$all" > "$d/smp_affinity_list" 2>/dev/null || true
  done
  systemctl enable --now irqbalance 2>/dev/null && ok "irqbalance включён" \
    || warn "irqbalance не установлен"
}

dispatch "${1:-status}"
