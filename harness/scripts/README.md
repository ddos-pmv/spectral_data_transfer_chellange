# scripts

Подкоманда по умолчанию — `status`, она ничего не меняет.
`apply`/`bind` требуют root. Переменная окружения перебивает `host.conf`.

## Настройка машины

| | |
|---|---|
| `build.sh` | разовая сборка DPDK и `igb_uio`. `DPDK_VER=v25.11.2` `SRC=/opt/src` `WITH_IGB_UIO=1` |
| `host.sh` | `status` \| `apply` \| `revert` — зовёт cpu → mem → irq → nic |
| `cpu.sh` | `status` \| `apply` \| `revert` — гасит `OFFLINE_CPUS`, ставит `GOVERNOR`, `NO_TURBO` |
| `mem.sh` | `status` \| `apply` \| `revert` — `HUGEPAGES_2M` страниц по 2 МБ |
| `irq.sh` | `status` \| `apply` \| `revert` — все IRQ на `IRQ_CPUS`, `irqbalance` off |
| `nic.sh` | `status` \| `bind` \| `unbind` — карта между `DPDK_DRIVER` и `KERNEL_DRIVER` |
| `checkenv.sh` | GRUB, DPDK, бинари, часы — то, что скриптом не чинится |

`nic.sh status` показывает `PEER_MAC`/`PEER_IP` и Write Combining (проверять во
время прогона).

## Прогон

| | |
|---|---|
| `rx.sh` | приём. `COUNT=1000000` `WARMUP=20000` `IDLE_MS=2000` `SLOTS=1024` `BURST=32` |
| `tx.sh` | отправка. `RATE=200000` `TYPE=mixed` `SLOTS=1024` `BURST=32` `SRC_IP=` |

`rx.sh` первым, `tx.sh` вторым. Карта, ядра и порт — из `host.conf`.

```bash
sudo ./scripts/rx.sh
sudo IDLE_MS=60000 COUNT=3000000 ./scripts/rx.sh
sudo RATE=500000 ./scripts/tx.sh
```

## host.conf

`cp host.conf.example host.conf`. В `.gitignore` — у каждой машины свой.

| | |
|---|---|
| `DPDK_IFACE` `DPDK_PCI` | карта под DPDK. Пустой PCI = вывести из имени |
| `DPDK_ENI_INDEX` | device-index ENI для чтения MAC/IP из метаданных |
| `DPDK_DRIVER` `KERNEL_DRIVER` | `igb_uio` / `ena` |
| `ISOL_CPUS` | только сверка с `/proc/cmdline`, не применяется |
| `IRQ_CPUS` `OFFLINE_CPUS` | куда IRQ, что погасить |
| `GOVERNOR` `NO_TURBO` | пусто = не трогать (на EC2 недоступны) |
| `HUGEPAGES_2M` | 256 = 512 МБ |
| `CPU_RECEIVER` `CPU_CONSUMER` `CPU_SENDER` `CPU_PRODUCER` `PORT` | раскладка прогона |
| `PEER_MAC` `PEER_IP` | вторая машина, из её `nic.sh status` |

## После перезагрузки

```bash
sudo ./scripts/host.sh apply
```

## Замер со снятием смещения часов

Замер смещения часов встроен в `rx.sh` и `tx.sh` — отдельных шагов нет.
Отключается `CLOCK=0`.

```bash
# приёмник                              # отправитель
sudo ./scripts/rx.sh                    sudo RECEIVERS=N RATE=R ./scripts/tx.sh
```

`tx.sh` в конце печатает готовую строку:

```bash
./scripts/summarize.py results/rx-*/latencies.csv --offset A --offset-end B
```

Каждый скрипт обрамляет свой прогон замером: `rx.sh` поднимает отражатель до и
после, `tx.sh` до и после ходит к нему пробником. chrony останавливается и
обратно не включается — часы должны расходиться линейно всю серию
(вернуть: `sudo systemctl start chrony`). Нужен `PROBE_PEER` в `host.conf` —
адрес приёмника на управляющем интерфейсе.

## Прочее

`summarize.py` — перцентили по CSV, с необязательной коррекцией часов.
`fetch-results.sh` — забрать результаты со стенда.
