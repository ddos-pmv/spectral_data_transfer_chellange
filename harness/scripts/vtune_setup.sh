#!/bin/bash

# Проверяем, что скрипт запущен с правами root
if [ "$EUID" -ne 0 ]; then
  echo "Ошибка: Этот скрипт нужно запускать через sudo!"
  echo "Используйте: sudo $0"
  exit 1
fi

echo "===> 1. Применяем настройки для текущей сессии..."
sysctl -w kernel.perf_event_paranoid=0
sysctl -w kernel.kptr_restrict=0
sysctl -w kernel.yama.ptrace_scope=0

echo "===> 2. Сохраняем настройки для автозапуска после перезагрузки..."
CONFIG_FILE="/etc/sysctl.d/99-perf-vtune.conf"

# Записываем все параметры в файл конфигурации
echo "kernel.perf_event_paranoid=0" > "$CONFIG_FILE"
echo "kernel.kptr_restrict=0" >> "$CONFIG_FILE"
echo "kernel.yama.ptrace_scope=0" >> "$CONFIG_FILE"

echo "Конфигурация сохранена в $CONFIG_FILE"

echo "===> 3. Проверяем, что применилось..."
for key in perf_event_paranoid kptr_restrict; do
  printf '  kernel.%-20s = %s\n' "$key" "$(sysctl -n "kernel.$key")"
done

# Трейспоинты живут не в perf_event_paranoid, а в правах на tracefs, и без них
# перестают работать perf sched, off-CPU профилирование и трассировка syscall'ов.
# bench.py их не использует именно поэтому.
if [ -r /sys/kernel/tracing/events ]; then
  echo "  tracefs                        : доступен (perf sched / off-CPU работают)"
else
  echo "  tracefs                        : НЕДОСТУПЕН — perf sched, off-CPU и"
  echo "                                   трассировка syscall'ов работать не будут."
  echo "                                   Починка: mount -t tracefs none /sys/kernel/tracing"
  echo "                                   и chmod -R o+rX /sys/kernel/tracing"
fi

echo "===> Готово! Все настройки успешно применены."