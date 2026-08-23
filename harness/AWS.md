# Запуск на AWS: от пустого инстанса до замера

Инструкция рассчитана на человека, который видит этот репозиторий впервые. Всё, что
ниже, выполняется **на двух инстансах** (отправитель и получатель), если явно не
сказано иначе. Проверено на Ubuntu 24.04, DPDK 25.11.2.

Быстрая проверка состояния машины на любом шаге:

```bash
./scripts/checkenv.sh          # ничего не меняет, только смотрит и подсказывает
```

---

## 0. Что именно запускается

```
инстанс A                                инстанс B
┌──────────────────────────┐             ┌──────────────────────────┐
│ producer ──shm──▶ sender │──── UDP ───▶│ receiver ──shm──▶ consumer│
└──────────────────────────┘   VPC / L2  └──────────────────────────┘
     ./scripts/tx.sh                          ./scripts/rx.sh
```

`producer` и `consumer` — эталонные бинарники из задания, они не менялись.
`dpdk_sender` и `dpdk_reciever` — собственно решение. Замер задержки делает
`consumer` по полям `seq_id` и `send_ts_ns`, которые проставляет `producer` и
которые транспорт переносит **байт в байт**.

Есть два режима работы сети. Начните с **режима Б**, он проще и не отбирает SSH;
на режим А переходите ради последних микросекунд.

| | режим А: ENA PMD | режим Б: AF_XDP |
|---|---|---|
| драйвер | `net_ena`, карта отвязана от ядра | `net_af_xdp` поверх живого интерфейса |
| нужна вторая ENI | **да** (иначе потеряете SSH) | нет |
| копий пакета | ноль, DMA прямо в mbuf | одна, ядро копирует в umem |
| LLQ | да | нет |
| риск закрыть себе доступ | есть | нет |

---

## 1. Инстансы и сеть

**Типы.** `m7i.xlarge` / `m8a.xlarge` — минимум. Отдельноя ядро под producer, sender, receiver, consumer.

**Размещение.** Оба инстанса — в одной AZ, одной подсети и **обязательно в одной
cluster placement group**. Это единственный способ гарантировать, что машины стоят
в одной стойке; без неё разброс сетевой задержки перекроет всё, что делает код.

```bash
aws ec2 create-placement-group --group-name hft --strategy cluster
```

**Security group.** Трафик режется фабрикой VPC ещё до ENI, поэтому XDP и DPDK тут
не помогают. Нужны два правила:

- входящий TCP 22 — с вашего адреса;
- входящий **UDP 40001** — от второго инстанса (проще всего: source = сама эта SG).

**Вторая ENI (только для режима А).** Первая ENI остаётся ядру и держит SSH, вторая
отдаётся DPDK. Создать в той же подсети и той же SG, приаттачить к инстансу.

```bash
aws ec2 create-network-interface --subnet-id subnet-XXX --groups sg-XXX \
    --description dpdk
aws ec2 attach-network-interface --network-interface-id eni-XXX \
    --instance-id i-XXX --device-index 1
```

**Anti-spoofing.** VPC отбрасывает пакеты, у которых source MAC/IP не совпадают с
адресами ENI, с которой они уходят. Отсюда два следствия: отправитель обязан
подставлять родные MAC и IP своей ENI (`tx.sh` берёт IP с интерфейса сам), а
мультикаст и промискуитет в VPC не работают вовсе — рассылка на N получателей
делается репликацией unicast'ом. Отключать «Source/destination check» не требуется:
мы пользуемся собственными адресами интерфейса.

---

## 2. Установка одной командой

На **чистом** инстансе всё, что нужно для сборки и запуска, ставится скриптом:

```bash
git clone <репозиторий> && cd <репозиторий>/harness
sudo ./scripts/aws-setup.sh
```

Он ставит пакеты, собирает и устанавливает DPDK 25.11.2, прописывает библиотеки в
`ldconfig`, выделяет hugepages и собирает `igb_uio` с Write Combining. Повторный
запуск пропускает уже сделанное. Занимает несколько минут — основное время уходит
на компиляцию DPDK.

Переменные, если нужно отступить от умолчаний:

```bash
sudo DPDK_VER=v25.11.2 HUGEPAGES_2M=4096 ./scripts/aws-setup.sh
sudo WITH_IGB_UIO=0 ./scripts/aws-setup.sh     # только режим Б (AF_XDP)
```

Дальше — `make -j$(nproc)` и `./scripts/checkenv.sh`.

Разделы 3-4 ниже расписывают то же самое вручную: они нужны, если скрипт где-то
споткнулся или если нужно понять, что именно он сделал.

---

## 3. То же самое вручную

### 3.1 Пакеты

```bash
sudo apt update
sudo apt install -y build-essential git pkg-config meson ninja-build \
     python3-pyelftools libnuma-dev ethtool chrony \
     linux-headers-$(uname -r) linux-tools-$(uname -r) linux-tools-common
# для режима Б (AF_XDP):
sudo apt install -y libbpf-dev libxdp-dev
```

### 3.2 Сборка DPDK

```bash
git clone https://github.com/DPDK/dpdk-stable.git
cd dpdk-stable && git checkout v25.11.2
meson setup build --prefix=/usr/local -Dplatform=native -Dbuildtype=release
ninja -C build
sudo ninja -C build install
```

**`ldconfig` обязателен, а не «на всякий случай».** DPDK линкуется динамически, и
драйверы (`net_ena`, `net_af_xdp`) живут отдельными `.so` в `pmds-26.0/`, которые EAL
подгружает через `dlopen` уже в рантайме. Если каталог не в кэше линковщика, EAL
молча не найдёт ни одного драйвера и вы получите «no available ethernet devices».

```bash
echo /usr/local/lib/x86_64-linux-gnu | sudo tee /etc/ld.so.conf.d/dpdk.conf
sudo ldconfig
```

Проверка:

```bash
pkg-config --modversion libdpdk                       # 25.11.x
ls /usr/local/lib/x86_64-linux-gnu/dpdk/pmds-26.0/ | grep -E 'ena|af_xdp'
```

### 3.3 Про драйвер ENA и репозиторий amzn-drivers

Тут легко запутаться, поэтому по пунктам.

**ENA PMD собирать отдельно не нужно.** Он входит в дерево DPDK и ставится вместе с
ним — это `librte_net_ena.so` в списке выше. Разделы «Manual patching» и «Patching
using script» в
[документации amzn-drivers](https://github.com/amzn/amzn-drivers/blob/master/userspace/dpdk/README.md)
относятся к **старым LTS-веткам** DPDK (v18.11, v19.11, v20.11), куда бэкпортируют
более свежий ENA PMD. Мы собираем v25.11 — там ENA PMD новее любого бэкпорта, и
патчи не только не нужны, но и не приложатся.

**Кернеловый модуль `ena`** уже есть в Ubuntu и обслуживает первую ENI, через
которую идёт SSH. Пересобирать его из amzn-drivers имеет смысл, только если нужна
функция новее, чем в поставке дистрибутива (`modinfo ena | head -3`).

**А вот что из amzn-drivers действительно может понадобиться — это патч vfio для
Write Combining.** Подробности в разделе 8: без WC режим LLQ не просто не ускоряет,
а замедляет. Обойти это можно и без amzn-drivers, взяв `igb_uio` — что и делает
`aws-setup.sh`.

**Параметры ENA PMD в DPDK 25.11** (взяты из самой библиотеки, а не из документации:
`strings librte_net_ena.so | grep PMD_INFO`):

```
llq_policy=<0|1|2|3>   0 — выключить, 1 — как рекомендует устройство (умолчание),
                       2 — обычный заголовок LLQ, 3 — расширенный
miss_txc_to=<uint>     таймаут по потерянному TX-completion
control_path_poll_interval=0|<500-1000>
enable_frag_bypass=<0|1>
```

Осторожно при чтении документации amzn-drivers: там описаны `large_llq_hdr=1` и
`enable_llq=0|1` — это **старые** имена. В 25.11 они заменены одним `llq_policy`,
где `0` соответствует прежнему `enable_llq=0`, а `3` — прежнему `large_llq_hdr=1`.

---

## 4. Настройка ОС под минимальный хвост

Всё в этом разделе — про **предсказуемость**, а не про среднюю скорость. Средняя
задержка от изоляции ядер меняется мало, а p99.9 и p99.99 меняются в разы.

### 4.1 Параметры ядра

Отредактируйте `/etc/default/grub`, строка `GRUB_CMDLINE_LINUX_DEFAULT`. Пример для
машины с 4 vCPU, где ядра 2 и 3 отдаются транспорту:

```
isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 irqaffinity=0,1
default_hugepagesz=1G hugepagesz=1G hugepages=4
intel_idle.max_cstate=1 processor.max_cstate=1 idle=poll
transparent_hugepage=madvise
```

```bash
sudo update-grub && sudo reboot
```

Что здесь что:

- **`isolcpus`** убирает с ядер *задачи*. **`irqaffinity`** убирает с них
  *прерывания*. Это разные вещи, и одного `isolcpus` недостаточно — очень частая
  ошибка. Оба списка должны быть согласованы.
- **`nohz_full`** гасит планировочный тик на изолированных ядрах: без него каждое
  ядро получает прерывание 250-1000 раз в секунду, и оно исправно видно в хвосте.
- **`rcu_nocbs`** уносит RCU-колбэки на housekeeping-ядра.
- **Hugepages по 1 ГБ.** 2 МБ тоже работают, но TLB покрывает больше памяти
  большими страницами, а промах TLB на горячем пути — это сотни тактов. 1 ГБ
  выделяется только через командную строку ядра: на живой системе непрерывной
  физической памяти такого размера уже не найти.
- **C-states.** `idle=poll` не даёт ядру уходить в сон, из которого выход стоит
  десятки микросекунд. Это самый заметный одиночный параметр для p99.99.
  Плата — постоянное энергопотребление; на EC2 вы за него уже заплатили.
  На малых типах инстансов глубокие C-states могут быть недоступны в принципе,
  тогда параметр просто ничего не изменит.
- **THP в `madvise`.** При `always` фоновая дефрагментация (`khugepaged`)
  периодически останавливает процесс — ровно те выбросы, которые портят p99.99.

Отдельно про `mitigations=off`: он даёт заметный выигрыш на syscall-heavy пути (а
AF_XDP именно такой), но это осознанное снижение защищённости машины. **По умолчанию
мы его не включаем.** Если жюри готово прогнать с ним — упомяните это как отдельный
вариант, числа стоит показывать обоими способами.

### 4.2 Прерывания

```bash
sudo systemctl stop irqbalance && sudo systemctl disable irqbalance
```

`irqbalance` живёт ровно для того, чтобы размазывать прерывания по всем ядрам, то
есть работает против нас. После его остановки посмотрите, что осталось:

```bash
./scripts/checkenv.sh              # секция «Прерывания»
sudo ./scripts/irq.py 30 2,3       # дельта за 30 секунд, а не с загрузки
```

Подвижные прерывания переносятся на housekeeping-ядра:

```bash
echo 0-1 | sudo tee /proc/irq/<N>/smp_affinity_list
```

Managed MSI-X (очереди NVMe, очереди самой ENA) так не переносятся — их закрывает
только `irqaffinity=` в командной строке ядра.

### 4.3 Прочее

```bash
sudo sh -c 'echo madvise > /sys/kernel/mm/transparent_hugepage/enabled'
sudo cpupower frequency-set -g performance   # на EC2 может отсутствовать — это нормально
ulimit -l unlimited                          # memlock под umem AF_XDP, в сессии запуска
```

Проверка hugepages после перезагрузки:

```bash
grep -E 'HugePages_(Total|Free)|Hugepagesize' /proc/meminfo
```

---

## 5. Синхронизация часов — без этого замер недействителен

`send_ts_ns` ставится на **одной** машине, а разница считается на **другой**. Любой
сдвиг между их часами уезжает прямо в результат, и никакая оптимизация транспорта
этого не исправит. На AWS есть аппаратный PTP-таймер, доступный через ENA.

```bash
ethtool -T ens5 | head          # ищем PTP Hardware Clock: 0 (а не none)
ls -l /dev/ptp0
```

Подключить его как источник времени для chrony:

```bash
sudo tee -a /etc/chrony/chrony.conf <<'EOF'
refclock PHC /dev/ptp0 poll 0 dpoll -2 offset 0 stratum 1
EOF
sudo systemctl restart chrony
sleep 30 && chronyc sources -v && chronyc tracking
```

В `chronyc tracking` интересует `System time` — должно быть в пределах единиц
микросекунд. Если PHC недоступен, оставьте штатный Amazon Time Sync
(`169.254.169.123`) и **обязательно** отметьте это в отчёте: точность там на порядок
хуже, и разница между p50 двух прогонов может оказаться артефактом часов.

Независимая проверка, не зависящая от синхронизации вовсе: измерить RTT (`ping -c
1000 -i 0.001`) и сравнить половину медианы с односторонней задержкой из `consumer`.
Если half-RTT и измеренная задержка расходятся систематически — виноваты часы.

---

## 6. Сборка проекта

На обоих инстансах:

```bash
git clone <репозиторий> && cd <репозиторий>/harness
make -j$(nproc)
ls bin/          # producer consumer dpdk_sender dpdk_reciever dpdk_probe
```

Цели `dpdk_*` собираются, только если `pkg-config` видит `libdpdk`; если их нет в
`bin/`, вернитесь к шагу 3. Полезно сразу проверить, что DPDK видит железо:

```bash
sudo ./bin/dpdk_probe -l 0 --vdev net_af_xdp0,iface=ens5,start_queue=0,queue_count=1
```

---

## 7. Режим Б: AF_XDP (рекомендуется начать с него)

Карта остаётся у ядра, SSH никуда не девается. Один раз на каждом инстансе:

```bash
sudo ethtool -L ens5 combined 1              # AF_XDP слушает очередь 0; RSS уведёт поток мимо
sudo ethtool -K ens5 gro off gso off tso off # сегментация ломает сырой путь
```

Запуск — **сначала приёмная машина**:

```bash
# инстанс B
sudo ./scripts/rx.sh
# напечатает PEER_MAC и DST_IP — их взять для tx.sh
```

```bash
# инстанс A
sudo PEER_MAC=06:aa:bb:cc:dd:ee DST_IP=172.31.5.20 RATE=200000 ./scripts/tx.sh
```

`rx.sh` завершится сам, набрав `COUNT` замеров, и допечатает статистику приёмника.
`tx.sh` останавливается по Ctrl+C, оба процесса при этом печатают свои счётчики.

---

## 8. Режим А: ENA PMD

Делается **только на второй ENI**. Первая остаётся ядру и держит SSH.

### 8.1 Write Combining — сначала прочитать это

`m7i` и `m8a` — инстансы 7-го и 8-го поколения, а начиная с 6-го **LLQ не просто
рекомендован, а обязателен**: без него производительность ENA падает драматически.
При этом LLQ требует, чтобы memory BAR устройства был отображён как **Write
Combining**, и ни `igb_uio`, ни `vfio-pci` по умолчанию этого не делают. Без WC
запись в память устройства идёт медленными несгруппированными PCI-транзакциями, и
LLQ превращается из ускорителя в тормоз.

Отсюда два рабочих пути:

| | `igb_uio` с `wc_activate=1` | `vfio-pci`, пересобранный с WC |
|---|---|---|
| откуда | `dpdk-kmods` (собирает `aws-setup.sh`) | `amzn-drivers/userspace/dpdk/enav2-vfio-patch` |
| что делает | модуль вне дерева ядра | пересобирает `vfio` из исходников ядра |
| IOMMU | не поддерживает | поддерживает |
| сложность | `make` и `insmod` | скачивание исходников ядра, пересборка, замена модуля |

**Мы используем `igb_uio`.** На обычных (не `*.metal`) инстансах EC2 IOMMU гостю не
отдаётся, так что его отсутствие в `igb_uio` ничего не стоит, а пересборка ядерного
модуля на каждом инстансе — лишняя точка отказа. Если по каким-то причинам нужен
именно `vfio-pci`, второй путь описан у Amazon:

```bash
git clone https://github.com/amzn/amzn-drivers.git
cd amzn-drivers/userspace/dpdk/enav2-vfio-patch && sudo ./get-vfio-with-wc.sh
```

Учтите: на некоторых образах Ubuntu `vfio-pci` вкомпилирован в ядро, а не собран
модулем, и тогда скрипт не сработает без пересборки ядра. Проверить:

```bash
modinfo vfio-pci >/dev/null 2>&1 && echo "модуль — ок" || echo "вкомпилирован — нужен igb_uio"
```

### 8.2 Привязка карты

Перед отвязкой запишите MAC и IP второй ENI — после привязки интерфейс исчезнет из
`ip link`:

```bash
ip -o link show ens6; ip -4 -o addr show ens6
# либо из метаданных инстанса:
curl -s http://169.254.169.254/latest/meta-data/network/interfaces/macs/
```

```bash
sudo modprobe uio
sudo modprobe igb_uio                  # wc_activate=1 прописан в /etc/modprobe.d/
sudo ip link set ens6 down
dpdk-devbind.py --status               # найти PCI-адрес, обычно 0000:00:06.0
sudo dpdk-devbind.py --bind=igb_uio 0000:00:06.0
dpdk-devbind.py --status | head -20
```

Модуль не переживает перезагрузку — после reboot повторить `modprobe` и `--bind`.

### 8.3 Проверка, что Write Combining действительно включился

Это единственный способ убедиться, что LLQ работает как задумано, а не наоборот.
Запустите приложение и **не останавливая его** посмотрите в соседней сессии:

```bash
lspci -v -s 00:06.0 | grep prefetchable     # запомнить адрес, напр. fe900000
sudo cat /sys/kernel/debug/x86/pat_memtype_list | grep fe900000
```

Рядом с prefetchable-регионом должно стоять `write-combining`. Если стоит что-то
другое — WC не включился, и режим А в таком виде запускать не стоит: числа будут
хуже, чем в режиме Б.

### 8.4 Запуск

Теми же скриптами, через подмену устройства EAL:

```bash
# инстанс B
sudo EAL_DEV="-a 0000:00:06.0" ./scripts/rx.sh

# инстанс A
sudo EAL_DEV="-a 0000:00:06.0" \
     PEER_MAC=<MAC второй ENI на B> DST_IP=<IP второй ENI на B> ./scripts/tx.sh
```

**Про `llq_policy` без аргументов — то есть значение по умолчанию, `1`.** Обычный
LLQ-заголовок вмещает 96 байт, расширенный (`3`) — 224, но ценой вдвое более
короткой TX-очереди. Наши L2/L3/L4 заголовки занимают 42 байта и в обычный
помещаются с запасом, так что `3` за них ничего не даёт. Единственное, ради чего его
стоит попробовать, — короткие сообщения: `Trade` это 42 + 72 = 114 байт, и они
целиком влезают в расширенную запись, то есть устройству вообще не придётся
DMA-читать тело. Это гипотеза, её надо мерить: прогоните `llq_policy=1` и
`llq_policy=3` и сравните.

Диагностика самого PMD, если что-то не поднимается:

```bash
sudo ./bin/dpdk_sender -a 0000:00:06.0 --log-level=pmd.net.ena.*:8 ...
```

В логе ищите строки `LLQ policy is ...` и `LLQ entry size`. Строка
`LLQ is not supported. Fallback to host mode policy` означает, что LLQ не
включился — возвращайтесь к 8.1.

Вернуть карту ядру:

```bash
sudo dpdk-devbind.py --bind=ena 0000:00:06.0 && sudo ip link set ens6 up
```

---

## 9. Что должно получиться и как понять, что что-то не так

`consumer` печатает распределение задержки, `dpdk_sender` и `dpdk_reciever` — свои
счётчики. Смотреть в первую очередь на них:

| счётчик | о чём говорит |
|---|---|
| `sent` = `received` | потерь на канале нет |
| `lapped` > 0 | отправитель не успевает за producer'ом, кольцо перезаписывается |
| `tx_dropped` > 0 | TX-кольцо переполнено, пакеты не ушли |
| `avg_burst` ≈ 1.0 | процесс простаивает, узкое место не здесь |
| `avg_burst` растёт | процесс насыщен, вот он и есть узкое место |

**`received: 0` при живом отправителе.** По убыванию вероятности: не открыт UDP 40001
в security group; поток попал не в нулевую очередь (`ethtool -L ens5 combined 1`);
неверный `PEER_MAC`; пакеты зарезаны anti-spoofing'ом из-за неверного `SRC_IP`.

**`no available ethernet devices`.** EAL не нашёл драйвер — почти всегда забытый
`ldconfig` (шаг 3) либо неверные аргументы в `--vdev`.

**`Cannot get hugepage information`.** Не выделены hugepages либо запуск не из-под
root.

**Хвост в десятки миллисекунд.** Проверьте, что прогон не идёт на burstable-инстансе
и что `checkenv.sh` не ругается на изоляцию, C-states и прерывания.

---

## 10. Сводка того, что настраивается, и зачем

Для отчёта и для быстрой сверки на новой машине:

| настройка | что чинит |
|---|---|
| cluster placement group | разброс сетевой задержки между инстансами |
| `isolcpus` + `nohz_full` + `rcu_nocbs` | вытеснение busy-spin процессов и тик планировщика |
| `irqaffinity` + выключенный irqbalance | прерывания на рабочих ядрах |
| `idle=poll`, C-states | выход из сна как источник выбросов p99.99 |
| hugepages 1 ГБ | промахи TLB на горячем пути |
| THP=madvise | остановки процесса от `khugepaged` |
| LLQ (ENA PMD, включён по умолчанию) | лишний DMA-round-trip на отправке |
| `igb_uio wc_activate=1` | без Write Combining LLQ замедляет, а не ускоряет |
| `ethtool -L combined 1` (AF_XDP) | поток мимо слушаемой очереди |
| PTP через `/dev/ptp0` + chrony | корректность самого измерения |
