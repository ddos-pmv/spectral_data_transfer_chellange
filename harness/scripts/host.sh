#!/usr/bin/env bash
# Вся настройка машины одной командой: зовёт cpu/mem/irq/nic по порядку.
#
#   ./scripts/host.sh              состояние всего
#   sudo ./scripts/host.sh apply   после каждой перезагрузки
#   sudo ./scripts/host.sh revert  вернуть машину в обычное состояние
#
# Порядок не случаен: сначала гасятся ядра (иначе IRQ уедут на ядро, которое
# сейчас выключат), потом память, потом прерывания, и только в конце карта --
# её отвязка ломает сеть на этом интерфейсе, и если что-то пойдёт не так,
# предыдущие шаги уже сделаны.
#
# Настройки GRUB (isolcpus, nohz_full, idle=poll, THP) сюда не входят: они
# требуют reboot. Их показывает ./scripts/checkenv.sh
set -euo pipefail
D=$(dirname "$(readlink -f "$0")")
. "$D/lib.sh"
SUBCOMMANDS="status apply revert"

cmd_status() { for m in cpu mem irq nic; do "$D/$m.sh" status; done; }
cmd_apply() {
  need_root apply
  "$D/cpu.sh" apply
  "$D/mem.sh" apply
  "$D/irq.sh" apply
  "$D/nic.sh" bind
  say "Готово"
  echo "   проверить остальное: ./scripts/checkenv.sh"
}
# Откат в обратном порядке: карту ядру раньше, чем включим обратно ядра и IRQ.
cmd_revert() {
  need_root revert
  "$D/nic.sh" unbind
  "$D/irq.sh" revert
  "$D/mem.sh" revert
  "$D/cpu.sh" revert
}

dispatch "${1:-status}"
