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
echo "===> Готово! Все настройки успешно применены."