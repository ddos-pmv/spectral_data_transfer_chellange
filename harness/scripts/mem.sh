#!/usr/bin/env bash
# Память: hugepages для DPDK и проверка THP.
#
#   ./scripts/mem.sh              сколько страниц, свободно, THP
#   sudo ./scripts/mem.sh apply   выделить HUGEPAGES_2M страниц
#   sudo ./scripts/mem.sh revert  вернуть 0
#
# DPDK держит mbuf-пулы в hugetlbfs: одна запись TLB покрывает 2 МБ вместо 4 КБ,
# поэтому обход пула не выбивает TLB. Это явные страницы, THP к ним отношения
# не имеет -- transparent_hugepage=madvise на них не влияет никак.
#
# THP важен для остального: khugepaged берёт mmap_lock, чтобы склеить страницы,
# и это видно как редкие выбросы в хвосте.
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/lib.sh"
SUBCOMMANDS="status apply revert"

HP_DIR=/sys/kernel/mm/hugepages/hugepages-2048kB

cmd_status() {
  say "Hugepages 2M (нужно $HUGEPAGES_2M)"
  local total free
  total=$(cat "$HP_DIR/nr_hugepages" 2>/dev/null || echo 0)
  free=$(awk '/HugePages_Free/ {print $2}' /proc/meminfo)
  row "выделено" "$total ($((total * 2)) МБ)"
  row "свободно" "$free"
  if [ "$total" -ge "$HUGEPAGES_2M" ]; then ok "хватает"
  else bad "мало -- sudo ./scripts/mem.sh apply"; fi
  # Смонтированный hugetlbfs нужен EAL, чтобы сделать файлы под пулы.
  if grep -q hugetlbfs /proc/mounts; then ok "hugetlbfs: $(awk '$3=="hugetlbfs"{print $2; exit}' /proc/mounts)"
  else warn "hugetlbfs не смонтирован (обычно /dev/hugepages есть по умолчанию)"; fi

  say "1G-страницы"
  local g; g=$(cat /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages 2>/dev/null || echo -)
  row "выделено" "$g"
  [ "$g" = 0 ] && row "" "включаются только через GRUB: default_hugepagesz=1G hugepagesz=1G hugepages=N"

  say "THP"
  local thp; thp=$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo "-")
  row "enabled" "$thp"
  case $thp in
    *'[madvise]'*|*'[never]'*) ok "khugepaged не лезет в чужие маппинги" ;;
    *'[always]'*) warn "always -- склейка страниц под mmap_lock попадёт в хвост" ;;
  esac
  row "khugepaged" "$(pgrep -c khugepaged 2>/dev/null || true) процессов"
}

cmd_apply() {
  need_root apply
  say "Hugepages: $HUGEPAGES_2M x 2M"
  echo "$HUGEPAGES_2M" > "$HP_DIR/nr_hugepages"
  local got; got=$(cat "$HP_DIR/nr_hugepages")
  # Ядро выделяет столько, сколько нашло непрерывных блоков: на живой системе
  # с фрагментированной памятью может дать меньше запрошенного.
  if [ "$got" -lt "$HUGEPAGES_2M" ]; then
    warn "выделено $got из $HUGEPAGES_2M -- память фрагментирована"
  else
    ok "выделено $got"
  fi
  grep -E 'HugePages_(Total|Free)' /proc/meminfo | sed 's/^/   /'
}

cmd_revert() {
  need_root revert
  say "Освобождаю hugepages"
  echo 0 > "$HP_DIR/nr_hugepages"
  ok "nr_hugepages=0"
}

dispatch "${1:-status}"
