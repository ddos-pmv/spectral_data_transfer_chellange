# Запуск бенчмарка на AWS

Две машины `m7i.xlarge`, Ubuntu 24.04, одна подсеть, одна AZ.
У каждой **вторая ENI** (`device-index 1`) — по ней идёт трафик, `ens5` остаётся под SSH.
Security group второй ENI должна пропускать UDP/40001 от самой себя.

Всё ниже выполняется **на обеих машинах**, если не сказано иное.

## 1. Установка

```bash
git clone <репозиторий> && cd <репозиторий>/harness
sudo ./scripts/build.sh
make -j$(nproc)
```

## 2. Параметры ядра

```bash
sudo tee /etc/default/grub.d/99-hft.cfg <<'CFG'
GRUB_CMDLINE_LINUX_DEFAULT="$GRUB_CMDLINE_LINUX_DEFAULT isolcpus=1,2 nohz_full=1,2 rcu_nocbs=1,2 irqaffinity=0 idle=poll processor.max_cstate=1 intel_idle.max_cstate=1 transparent_hugepage=madvise"
CFG
sudo update-grub
sudo grep hft /boot/grub/grub.cfg      # строка должна быть на месте
```

```bash
sudo reboot
```
```bash
grep isolcpus /proc/cmdline
```

## 3. Конфиг

```bash
cp scripts/host.conf.example scripts/host.conf
```

Поправить `DPDK_IFACE` — имя второй ENI из `ip a`, обычно `enp40s0`.
Остальное менять не нужно.

## 4. Настройка машины

```bash
sudo ./scripts/host.sh apply
./scripts/checkenv.sh
```

Повторять **после каждой перезагрузки и после `stop`/`start` инстанса**:
привязка карты и настройки ядер живут только до выключения.

## 5. Адреса

На **приёмной** машине:

```bash
./scripts/nic.sh status
```

Выписать `PEER_MAC` и `PEER_IP` и вписать их в `scripts/host.conf`
на **отправляющей** машине.

## 6. Прогон

Приёмная машина, первой:

```bash
sudo IDLE_MS=60000 COUNT=1000000 ./scripts/rx.sh
```

Отправляющая машина, второй:

```bash
sudo ./scripts/tx.sh
```

`rx.sh` завершается сам, набрав `COUNT` замеров, и печатает распределение
задержки. Логи — в `results/rx-<дата>/`.

Другой темп: `sudo RATE=500000 ./scripts/tx.sh`

## 7. Точность часов

Задержка считается как разница отметок времени двух машин, поэтому расхождение
их часов входит в результат целиком. Перед серией замеров остановить синхронизацию
на **обеих** машинах:

```bash
sudo systemctl stop chrony
```

Прогнать в обе стороны, поменяв машины ролями. Истинная задержка — `(AB + BA) / 2`,
расхождение часов — `(AB − BA) / 2`, по каждому процентилю отдельно. После замеров:

```bash
sudo systemctl start chrony
```

## Вернуть машину в обычное состояние

```bash
sudo ./scripts/host.sh revert
```

---

Подробности — `AWS.md`. Описание скриптов — `scripts/README.md`.
