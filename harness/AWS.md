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

Сеть работает через **ENA PMD**: карта отвязывается от ядра и отдаётся DPDK.
Пакет не копируется — DMA идёт прямо в mbuf, `tx_burst` пишет в MMIO мимо ядра,
работает LLQ. Ровно поэтому нужна **вторая ENI**: отвязать ту, на которой висит
SSH, значит потерять доступ к машине.

Без AWS то же самое поднимается локально на паре `veth` с драйвером `net_af_xdp`
вместо `net_ena` — код транспорта при этом не меняется, меняется только аргумент
EAL.

---

## 1. Инстансы и сеть

**Типы.** `m7i.xlarge` / `m8a.xlarge` — практический минимум, `.2xlarge` —
для финальных цифр. Размер важнее поколения, и вот почему.

- **Очереди ENA.** Число очередей, которое карта отдаёт гостю, растёт с размером
  инстанса. На `.large` их **одна** (`ethtool -l ens5`, строка `Combined` в разделе
  «Pre-set maximums»). Проверяйте сразу после запуска.
- **PPS.** Nitro ограничивает не только полосу, но и число пакетов в секунду, и
  лимит тоже растёт с размером. Упереться в него — значит получить не дропы, а рост
  хвоста, что диагностируется куда хуже.
- **Burst-кредиты сети.** У малых размеров в таблице AWS написано «Up to N Gbps» —
  полоса кредитная, и через несколько минут прогона её режут. Для замера хвоста это
  яд: деградация выглядит как «код стал хуже».
- **Физические ядра, а не vCPU.** У Intel-инстансов vCPU — это поток SMT:
  `m7i.xlarge` = 4 vCPU = **2 физических ядра**. Два busy-spin процесса на двух
  потоках одного ядра делят L1, L2 и исполнительные блоки. Пары смотреть в
  `lscpu -e=CPU,CORE,SOCKET` — сиблинги имеют одинаковый `CORE`.

Гипертрединг чище всего гасится не руками, а на уровне инстанса — **CPU options**,
`ThreadsPerCore=1`. Тогда каждый видимый vCPU это целое ядро, и не надо ни разбираться
в нумерации, ни писать в `/sys/devices/system/cpu/cpuN/online`. Меняется только на
остановленном инстансе и **на квоту с ценой не влияет** — платите за полный тип.

**Никогда не берите семейство T** (t3, t3a, t4g): это burstable CPU, при исчерпании
кредитов машину троттлят по частоте прямо посреди прогона.

**Квота vCPU.** У аккаунта есть лимит на суммарное число vCPU **запущенных**
инстансов; у новых он часто равен 8, то есть две `.2xlarge` уже не влезают:

```
You have requested more vCPU capacity than your current vCPU limit of 8 allows
```

Лимит считается в vCPU, а не в инстансах, и остановленные машины в него не входят.
Проверить и запросить:

```bash
aws service-quotas get-service-quota --service-code ec2 \
    --quota-code L-1216C47A --region <регион> --query 'Quota.Value'

aws service-quotas request-service-quota-increase --service-code ec2 \
    --quota-code L-1216C47A --desired-value 32 --region <регион>
```

Квота региональная. История запросов — Service Quotas → Quota request history;
статус `CASE_OPENED` означает, что вопрос ушёл человеку и в Support Center вас
может ждать вопрос.

**Резайз** делается только на остановленном инстансе, диск и приватный IP при этом
сохраняются, а публичный IP меняется (если нет Elastic IP). Так что всю установку
можно сделать на машине поменьше, а на большую перейти перед замерами:

```bash
aws ec2 modify-instance-attribute --instance-id i-XXX --instance-type m7i.2xlarge
```

**Размещение.** Оба инстанса — в одной AZ, одной подсети и **обязательно в одной
cluster placement group**. Это единственный способ гарантировать, что машины стоят
в одной стойке; без неё разброс сетевой задержки перекроет всё, что делает код.

```bash
aws ec2 create-placement-group --group-name hft --strategy cluster
```

Группу нужно создать **до** запуска инстансов и указать при запуске: задним числом
она применяется только к остановленной машине.

**Security group.** Трафик режется фабрикой VPC ещё до ENI, поэтому DPDK тут
не помогают. Нужны два правила:

- входящий TCP 22 — с вашего адреса;
- входящий **UDP 40001** — от второго инстанса (проще всего: source = сама эта SG).

```bash
aws ec2 authorize-security-group-ingress --group-id sg-XXX \
    --protocol udp --port 40001 --source-group sg-XXX
```

`--source-group` на саму себя означает «разрешить всем, кто состоит в этой же
группе»: правило не привязано к конкретным адресам и переживает пересоздание ENI.

**Security group привязана к ENI, а не к инстансу.** Вторая ENI получает ту группу,
которую вы указали при её создании, и правила первичной на неё не распространяются.
Это самая частая причина `received: 0` без единой ошибки в логах.

**Вторая ENI (только для режима А).** Первая ENI остаётся ядру и держит SSH, вторая
отдаётся DPDK. Создать в той же подсети и той же SG, приаттачить к инстансу.

```bash
ENI=$(aws ec2 create-network-interface --subnet-id subnet-XXX --groups sg-XXX \
    --description dpdk --query 'NetworkInterface.NetworkInterfaceId' --output text)
aws ec2 attach-network-interface --network-interface-id $ENI \
    --instance-id i-XXX --device-index 1

aws ec2 describe-network-interfaces --network-interface-ids $ENI \
    --query 'NetworkInterfaces[].{MAC:MacAddress,IP:PrivateIpAddress}' --output table
```

MAC и IP из последней команды — это то, что пойдёт в `PEER_MAC`, `DST_IP` и
`SRC_IP`. Запишите их сразу: после привязки к DPDK карта пропадёт из `ip a`.

---

## 2. Установка одной командой

На **чистом** инстансе всё, что нужно для сборки и запуска, ставится скриптом:

```bash
git clone <репозиторий> && cd <репозиторий>/harness
sudo ./scripts/build.sh
```

Он ставит пакеты, собирает и устанавливает DPDK 25.11.2, прописывает библиотеки в
`ldconfig` и собирает `igb_uio` с Write Combining. Повторный запуск пропускает уже
сделанное. Занимает несколько минут — основное время уходит на компиляцию DPDK.

Настройку самой машины он **не** делает: это отдельный шаг, см. 2.1.

Переменные, если нужно отступить от умолчаний:

```bash
sudo DPDK_VER=v25.11.2 ./scripts/build.sh
sudo WITH_IGB_UIO=0 ./scripts/build.sh     # без igb_uio (только локальный стенд)
```

### 2.1 Настройка машины

Состояние железа задаётся одним конфигом и применяется одной командой:

```bash
cp scripts/host.conf.example scripts/host.conf
$EDITOR scripts/host.conf        # PCI карты, какие ядра гасить, куда IRQ
sudo ./scripts/host.sh apply
```

Всё разбито на четыре скрипта с одинаковой грамматикой — `status` (по умолчанию,
ничего не меняет), `apply`, `revert`:

| скрипт | что делает |
|---|---|
| `cpu.sh` | гасит SMT-сиблинги, governor, `no_turbo`; сверяет `isolcpus` с конфигом |
| `mem.sh` | hugepages, проверка THP |
| `irq.sh` | сгоняет прерывания на housekeeping-ядро, выключает `irqbalance` |
| `nic.sh` | `bind`/`unbind` карты, адреса ENI, проверка Write Combining |
| `host.sh` | зовёт все четыре по порядку |

Голый запуск любого из них **никогда ничего не меняет** — только показывает
состояние. Это важно: `nic.sh` без аргументов не отвяжет карту случайно.

Крутить один фактор, не трогая остальные:

```bash
./scripts/nic.sh status          # PCI, драйвер, MAC, IP, Write Combining
sudo ./scripts/nic.sh unbind     # вернуть карту ядру
sudo ./scripts/cpu.sh revert     # включить ядра обратно, IRQ не трогать
./scripts/irq.sh                 # кто где сидит и сколько раз сработал
```

`scripts/host.conf` в `.gitignore`: PCI, MAC и адреса у двух машин разные. В
репозитории лежит `host.conf.example`. Переменная окружения перебивает файл:

```bash
OFFLINE_CPUS= sudo -E ./scripts/cpu.sh apply    # не гасить ничего
```
---

## 3. То же самое вручную

### 3.1 Пакеты

```bash
sudo apt update
sudo apt install -y build-essential git pkg-config meson ninja-build \
     python3-pyelftools libnuma-dev ethtool chrony \
     linux-headers-$(uname -r) linux-tools-$(uname -r) linux-tools-common
# для локального стенда на veth (AF_XDP):
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

Сборка собирает **все** драйверы и требовательна к памяти: на машине с 1-2 ГБ
`ninja` падает с `cc: fatal error: Killed signal terminated program cc1` — это OOM,
а не ошибка компиляции. Лечится либо инстансом побольше (проще), либо ограничением
набора драйверов и параллелизма:

```bash
meson setup build --prefix=/usr/local -Dplatform=native -Dbuildtype=release \
      -Denable_drivers=net/ena,net/af_xdp,net/null -Dtests=false
ninja -C build -j2
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
`build.sh`.

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
машины с 8 vCPU (4 физических ядра), где транспорту отдаются 2 и 3:

```
isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 irqaffinity=0,1
default_hugepagesz=1G hugepagesz=1G hugepages=4
intel_idle.max_cstate=1 processor.max_cstate=1 idle=poll
transparent_hugepage=madvise
```

**На облачных образах Ubuntu редактировать `/etc/default/grub` бесполезно.**
`update-grub` читает сначала его, а **потом** подключает `/etc/default/grub.d/*.cfg`,
и лежащий там `50-cloudimg-settings.cfg` переопределяет `GRUB_CMDLINE_LINUX_DEFAULT`
целиком. Правка молча пропадает: ни ошибки, ни предупреждения — просто в
`/proc/cmdline` не окажется ничего из добавленного, включая штатные `quiet splash`.
Это самый надёжный признак, что вы наступили именно на это.

Правильный способ — свой файл с номером побольше, дополняющий переменную:

```bash
sudo tee /etc/default/grub.d/99-hft.cfg <<'EOF'
GRUB_CMDLINE_LINUX_DEFAULT="$GRUB_CMDLINE_LINUX_DEFAULT isolcpus=1,2 nohz_full=1,2 rcu_nocbs=1,2 irqaffinity=0 idle=poll processor.max_cstate=1 intel_idle.max_cstate=1 transparent_hugepage=madvise"
EOF
```

`$GRUB_CMDLINE_LINUX_DEFAULT` в начале обязателен: без него потеряются
`console=ttyS0` (вывод в Serial Console — аварийный доступ к машине) и
`nvme_core.io_timeout`.

**Результат виден до перезагрузки** — проверяйте сгенерированный конфиг, а не
загружайтесь вслепую:

```bash
sudo update-grub
grep -o 'isolcpus=[^ ]*' /boot/grub/grub.cfg | head    # пусто = не применится
sudo reboot
```

Ядро **молча игнорирует неизвестные параметры** — опечатка вроде `rcu_nocvs` вместо
`rcu_nocbs` или `transparent_hugepages` вместо `transparent_hugepage` не вызовет ни
ошибки, ни записи в `dmesg`. Поэтому после перезагрузки проверяется не отсутствие
жалоб, а каждый параметр по отдельности:

```bash
cat /proc/cmdline                             # что ядро получило
cat /sys/devices/system/cpu/isolated          # isolcpus  -> 1-2
cat /sys/devices/system/cpu/nohz_full         # nohz_full -> 1-2
sudo dmesg | grep -i 'offload RCU'            # rcu_nocbs -> "from CPUs: 1-2"
cat /proc/irq/default_smp_affinity            # irqaffinity
cat /sys/kernel/mm/transparent_hugepage/enabled   # -> [madvise]
cat /sys/module/intel_idle/parameters/max_cstate  # -> 1
```

При действующем `idle=poll` подсистема `cpuidle` не используется вовсе, и каталог
`/sys/devices/system/cpu/cpu0/cpuidle/` может отсутствовать — здесь это признак
успеха, а не ошибки.

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

**Номера ядер выбирайте по `lscpu -e=CPU,CORE,SOCKET`, а не подряд.** У Intel-инстансов
vCPU — это поток SMT, и `2,3` вполне могут оказаться двумя потоками одного физического
ядра: тогда два busy-spin процесса поделят между собой один L1 и одни исполнительные
блоки, и изоляция даст обратный эффект. Изолировать нужно vCPU с **разными** `CORE`.
Радикальнее и надёжнее — выключить SMT на уровне инстанса через CPU options
(`ThreadsPerCore=1`, раздел 1): тогда каждый видимый vCPU это целое ядро, и вопрос
снимается совсем.

На машине с 4 vCPU (2 физических ядра) свободных ядер под изоляцию по сути нет: одно
обязано остаться системе. Такая конфигурация годится, чтобы убедиться, что всё
собирается и едет, но не для финальных чисел.

Отдельно про `mitigations=off`: он даёт заметный выигрыш на syscall-heavy пути,
но это осознанное снижение защищённости машины. **По умолчанию
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
ulimit -l unlimited                          # memlock, в сессии запуска
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
`bin/`, вернитесь к шагу 3. Проверить, что EAL вообще жив и находит драйверы, можно
на устройстве-заглушке — оно ничего не требует от железа:

```bash
sudo ./bin/dpdk_probe -l 0 --vdev net_null0,no-rx=1
```

Если вместо порта видите `no available ethernet devices` — драйверы не найдены,
почти всегда это забытый `ldconfig` из шага 3.2. После привязки карты (раздел 8) той
же командой с `-a <PCI>` проверяется уже реальное устройство.

---

## 7. Локальный стенд без AWS

Весь код отлаживается на одной машине: вместо сети между половинами цепочки
ставится **veth-пара**, а поверх её концов вместо `net_ena` поднимается
`net_af_xdp`. Аргумент EAL становится
`--vdev net_af_xdp0,iface=veth-rx,start_queue=0,queue_count=1`, всё остальное —
как на AWS. Команды поднятия стенда — в шапке `scripts/dpdk_local_bench.toml`.

Числа с такого стенда с AWS не сравнимы: AF_XDP на veth работает в copy-режиме,
и путь пакета там принципиально другой.

---

## 8. ENA PMD: привязка карты

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
| откуда | `dpdk-kmods` (собирает `build.sh`) | `amzn-drivers/userspace/dpdk/enav2-vfio-patch` |
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

Имя второй ENI внутри гостя не обязано быть `ens6`: ядро называет её по PCI-адресу,
и обычно получается что-то вроде `enp40s0`. Реальное имя и PCI-адрес смотрите в
`dpdk-devbind.py --status` — там же видно, у какого устройства `if=ens5`, то есть
какое трогать нельзя.

Перед отвязкой запишите MAC и IP этой ENI — после привязки интерфейс исчезнет из
`ip link`. Внутри инстанса их отдают метаданные (IMDSv2, нужен токен):

```bash
TOKEN=$(curl -s -X PUT -H 'X-aws-ec2-metadata-token-ttl-seconds: 300' \
        http://169.254.169.254/latest/api/token)
curl -s -H "X-aws-ec2-metadata-token: $TOKEN" \
     http://169.254.169.254/latest/meta-data/network/interfaces/macs/
```

`tx.sh` и `rx.sh` делают это сами — см. `scripts/lib.sh`.

```bash
sudo modprobe uio
sudo modprobe igb_uio                  # wc_activate=1 прописан в /etc/modprobe.d/
sudo ip link set enp40s0 down          # своё имя из dpdk-devbind.py --status
dpdk-devbind.py --status               # найти PCI-адрес нужной карты
sudo dpdk-devbind.py --bind=igb_uio 0000:28:00.0    # свой адрес из --status
dpdk-devbind.py --status | head -20
```

Устройство должно переехать в раздел «Network devices using DPDK-compatible driver»,
а интерфейс — пропасть из `ip a`. SSH на `ens5` при этом не моргает.

Модуль не переживает перезагрузку — после reboot повторить `modprobe` и `--bind`.
Это, кстати, встроенная страховка: если что-то пошло не так, достаточно перезагрузить
машину, и карта сама вернётся ядру.

**`igb_uio` собран под текущее ядро.** После `apt upgrade`, поставившего новое ядро,
и перезагрузки модуль перестанет грузиться (`modprobe: FATAL: Module igb_uio not
found`), и его нужно пересобрать — повторным запуском `build.sh` либо вручную из
`/opt/src/dpdk-kmods/linux/igb_uio`. На время замеров ядро лучше не обновлять.

Клонировать `dpdk-kmods` следует по HTTPS: канонический `git://dpdk.org/dpdk-kmods`
ходит по порту 9418, который в AWS обычно закрыт исходящими правилами.

```bash
git clone https://dpdk.org/git/dpdk-kmods
cd dpdk-kmods/linux/igb_uio && make
modinfo ./igb_uio.ko | grep parm      # должны быть intr_mode и wc_activate
```

### 8.3 Проверка, что Write Combining действительно включился

Проверять наличие `/sys/module/igb_uio/parameters/wc_activate` бесполезно: параметр
объявлен с правами доступа `0` и в sysfs не выставляется никогда, включён он или нет.
Виден только `intr_mode`, у которого права `S_IRUGO`.

Единственный способ убедиться, что LLQ работает как задумано, а не наоборот, —
посмотреть, каким типом ядро отобразило memory BAR. Запустите приложение и **не
останавливая его** посмотрите в соседней сессии:

```bash
lspci -v -s 00:06.0 | grep prefetchable     # запомнить адрес, напр. fe900000
sudo cat /sys/kernel/debug/x86/pat_memtype_list | grep fe900000
```

Рядом с prefetchable-регионом должно стоять `write-combining`. Если стоит что-то
другое — WC не включился, и режим А в таком виде запускать не стоит: числа будут
хуже, чем без DPDK вообще.

### 8.4 Запуск

Теми же скриптами, через подмену устройства EAL:

```bash
# инстанс B — напечатает PEER_MAC и DST_IP второй ENI, взять их для tx.sh
sudo EAL_DEV="-a 0000:28:00.0" ./scripts/rx.sh

# инстанс A
sudo EAL_DEV="-a 0000:28:00.0" \
     PEER_MAC=<MAC второй ENI на B> DST_IP=<IP второй ENI на B> ./scripts/tx.sh
```

PCI-адрес свой на каждой машине — берите из `dpdk-devbind.py --status`.

Ни MTU, ни `ethtool -L`, ни `gro off` здесь не нужны: всё это относится к AF_XDP, а
режим А идёт мимо ядра. `SRC_IP` скрипт тоже определит сам — через метаданные, а не
через `ip addr`, потому что карты в ядре больше нет.

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
sudo ./bin/dpdk_sender -a 0000:28:00.0 --log-level=pmd.net.ena.*:8 ...
```

В логе ищите строки `LLQ policy is ...` и `LLQ entry size`. Строка
`LLQ is not supported. Fallback to host mode policy` означает, что LLQ не
включился — возвращайтесь к 8.1.

Вернуть карту ядру:

```bash
sudo dpdk-devbind.py --bind=ena 0000:28:00.0 && sudo ip link set enp40s0 up
```

### 8.5 Что слетает при перезагрузке

Настройки из GRUB (`isolcpus`, `nohz_full`, `idle=poll`, THP) применяются сами.
Всё остальное живёт в памяти ядра и после `reboot` либо `stop`/`start` пропадает:

| | чем восстанавливается |
|---|---|
| hugepages | `mem.sh apply` |
| `igb_uio` и привязка карты | `nic.sh bind` |
| выключенный SMT-сиблинг | `cpu.sh apply` |
| аффинити прерываний, `irqbalance` | `irq.sh apply` |

То, что карта сама возвращается ядру после перезагрузки, — не баг, а встроенная
страховка: если привязка что-то сломала, машина чинится обычным `reboot`.

Всё это разом делает скрипт:

```bash
sudo ./scripts/host.sh apply
```

Он идемпотентен: повторный запуск на уже настроенной машине ничего не портит.

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

### Частые симптомы

**Прогон завершается сразу, `received: 0`, процессы гаснут.** Не поломка. У
`consumer` есть выход по простою: нет данных дольше `--idle-ms` (по умолчанию
**2000 мс**) — он считает, что producer закончил, и выходит, а `rx.sh` по `trap` гасит
приёмник. Если отправитель стартует позже, поднимите порог:
`sudo IDLE_MS=60000 ./scripts/rx.sh`. Сам `dpdk_reciever` по простою не выходит
никогда — его можно поднять заранее и оставить.

**`received: 0` при живом отправителе.** По убыванию вероятности:

1. UDP 40001 не открыт **в той SG, что висит на второй ENI** — группы привязаны к
   интерфейсу, а не к инстансу;
2. `PEER_MAC` / `DST_IP` взяты с первичного интерфейса, а не с той ENI, которую
   слушает DPDK (в режиме А `ip a` её уже не показывает — адреса берутся из
   метаданных, см. `scripts/lib.sh`);
3. пакеты зарезаны anti-spoofing'ом из-за неверного `SRC_IP`;
4. в режиме Б поток попал не в нулевую очередь (`ethtool -L <iface> combined 1`).

**`ena: Failed to set xdp program, the current MTU is larger than the maximum
allowed MTU`.** Режим Б на ENA. В VPC по умолчанию MTU 9001, а XDP требует, чтобы
кадр помещался в одну страницу за вычетом headroom — у ENA предел около 3498 байт.
Наши кадры максимум 258 байт, так что `sudo ip link set <iface> mtu 1500` безопасно.

**`ena: Failed to set xdp program, there is no enough space for allocating XDP
queues`.** Тоже режим Б. ENA под XDP резервирует по дополнительной TX-очереди на
каждую рабочую, то есть требует `рабочих × 2 ≤ максимума`. Смотрите `ethtool -l`:

```
Pre-set maximums:
Combined:    1        ← максимум, а не текущее значение
```

Если максимум равен единице — `1 × 2 = 2 > 1`, и уменьшать уже некуда: **XDP на этом
интерфейсе не поднимется никогда**. Число очередей растёт с размером инстанса,
поэтому лечится либо инстансом побольше, либо переходом на режим А, который XDP не
использует вовсе.

**`vdev_probe(): failed to initialize net_null0`.** У `net_null` свой набор devargs —
только `size`, `copy`, `no-rx`. Обычно это остаток от строки `net_af_xdp`, которую
поправили не целиком.

**`no available ethernet devices`.** EAL не нашёл драйвер — почти всегда забытый
`ldconfig` (шаг 3.2) либо неверные аргументы в `--vdev`.

**`Cannot get hugepage information`.** Не выделены hugepages либо запуск не из-под
root.

**`cc: fatal error: Killed signal terminated program cc1` при сборке DPDK.** Это OOM,
а не ошибка компиляции — см. 3.2.

**`modprobe: FATAL: Module igb_uio not found`.** Обновилось ядро, модуль надо
пересобрать — см. 8.2.

**Хвост в десятки миллисекунд.** Проверьте, что прогон не идёт на burstable-инстансе
(семейство T) и что `checkenv.sh` не ругается на изоляцию, C-states и прерывания.
Отдельно проверьте `lscpu -e=CPU,CORE,SOCKET`: два busy-spin процесса могли оказаться
на сиблингах одного физического ядра.

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
| `ethtool -K tx off rx off` (veth) | фиктивные контрольные суммы на стенде |
| вторая ENI под DPDK | режим А без потери SSH |
| `ThreadsPerCore=1` (CPU options) | соседи по SMT в L1 горячего процесса |
| PTP через `/dev/ptp0` + chrony | корректность самого измерения |
