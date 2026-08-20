#!/bin/bash

# Проверка прав (опционально, но список прерываний точнее от root)
if [ "$EUID" -ne 0 ]; then
    echo "⚠️ Рекомендуется запускать от root (sudo), чтобы увидеть все устройства."
fi

# Вывод красивого заголовка таблицы
printf "%-7s %-15s %-40s\n" "IRQ #" "CPU Core(s)" "Device / Driver"
printf "%s\n" "------------------------------------------------------------------"

# Перебор всех папок прерываний в /proc/irq/
for irq_dir in /proc/irq/[0-9]*; do
    # Извлекаем только номер IRQ из пути
    irq_num=$(basename "$irq_dir")

    # Проверяем существование файла со списком ядер
    if [ -f "$irq_dir/smp_affinity_list" ]; then
        # Читаем список ядер (например: 0-3 или 2)
        cores=$(cat "$irq_dir/smp_affinity_list")

        # Получаем имя устройства из /proc/interrupts по номеру IRQ
        # Берем только последнюю колонку с текстом
        device=$(awk -v irq="^ *${irq_num}:" '$1 ~ irq {for(i=NF; i>0; i--) if($i ~ /[A-Za-z]/) {print $i; break}}' /proc/interrupts)

        # Если имя устройства не найдено в /proc/interrupts, пишем [Unknown]
        if [ -z "$device" ]; then
            device="[Reserved / Idle]"
        fi

        # Выводим строку таблицы
        printf "%-7s %-15s %-40s\n" "$irq_num" "$cores" "$device"
    fi
done
