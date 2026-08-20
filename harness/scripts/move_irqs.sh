#!/bin/bash

# Скрипт должен запускаться от имени root
if [ "$EUID" -ne 0 ]; then
    echo "❌ Ошибка: Этот скрипт необходимо запускать с правами root (через sudo)."
    exit 1
fi

# Проверяем, передан ли аргумент (целевые ядра)
if [ -z "$1" ]; then
    echo "Использование: $0 <ядра>"
    echo "Примеры:"
    echo "  $0 2        (перенести все на ядро 2)"
    echo "  $0 2,4,6    (распределить по ядрам 2, 4 и 6)"
    echo "  $0 4-7      (распределить по ядрам с 4 по 7)"
    exit 1
fi

TARGET_CORES="$1"

# 1. Отключаем irqbalance, чтобы он не мешал ручной привязке
if systemctl is-active --quiet irqbalance; then
    echo "⏳ Обнаружен активный irqbalance. Останавливаем сервис..."
    systemctl stop irqbalance
fi

echo "========================================================"
echo " Начинаем перенос доступных IRQ на ядра: $TARGET_CORES"
echo "========================================================"

# Перебор всех папок прерываний
for irq_dir in /proc/irq/[0-9]*; do
    irq_num=$(basename "$irq_dir")
    affinity_file="$irq_dir/smp_affinity_list"

    # Проверяем, существует ли файл привязки и доступен ли он для записи
    if [ -f "$affinity_file" ] && [ -w "$affinity_file" ]; then

        # Получаем имя устройства для вывода информации
        device=$(awk -v irq="^ *${irq_num}:" '$1 ~ irq {for(i=NF; i>0; i--) if($i ~ /[A-Za-z]/) {print $i; break}}' /proc/interrupts)
        [ -z "$device" ] && device="[Unknown / Idle]"

        # Пытаемся записать новые ядра в файл affinity
        # Перенаправляем ошибки в /dev/null на случай, если ядро жестко заблокировало запись
        if echo "$TARGET_CORES" > "$affinity_file" 2>/dev/null; then
            # Проверяем, применились ли изменения
            current_cores=$(cat "$affinity_file")
            if [ "$current_cores" == "$TARGET_CORES" ]; then
                printf "✅ IRQ %-4s -> перенесено на CPU %-6s (%s)\n" "$irq_num" "$TARGET_CORES" "$device"
            else
                printf "⚠️  IRQ %-4s -> ядро частично изменило маску на %s (%s)\n" "$irq_num" "$current_cores" "$device"
            fi
        else
            printf "❌ IRQ %-4s -> перенос ЗАБЛОКИРОВАН ядром Linux (%s)\n" "$irq_num" "$device"
        fi
    fi
done

echo "========================================================"
echo " Перенос завершен."
echo " Чтобы вернуть всё обратно, перезапустите irqbalance: sudo systemctl start irqbalance"
