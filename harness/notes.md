aws instances:

What i need to do, before start bench:
1. taskset
2. isolcpus kernel boot parametr allows to specify a set of logical cores, wher user-space threads cannot be scheduled 
3. rcu_nocvs. specify a set of logical cores, which shuldn't run RCU callbacks
4. nohz_full/ specifies tickless behavior on marked cores
5. specify core to run IRQ callbacks for every IRQ:
   - cat /proc/interrupts to map devices to corresponding irq numbers
   - set a bitmask in  /proc/irq/$irq_num/smp_affinity
   - disable irqbalancec.service to prevent automatic balancing
6. split L3 cache
    - resctrl can be used to split L3 cache between diffenerent processes(cores)

# scripts/bench.py — тот же прогон по TOML-конфигу

`./bench.py [config.toml]` (по умолчанию `scripts/bench.toml`). Флагов нет: всё в конфиге, его копия кладётся рядом с результатами.

Отличия от `start_simple.sh`: ядра задаются явно в конфиге, а не подбираются; есть свипы и `summary.csv`; нет netem и автоподбора ядер.

|                  |                                                                                                                                 |
|------------------|---------------------------------------------------------------------------------------------------------------------------------|
| Свип             | Любое значение-список в `bench.fanout` или `producer.rate` разворачивается в декартово произведение. Скаляр = одна точка.       |
| `bench.fanout`   | Сколько секций `[[receiver]]` задействовать; берутся первые по порядку.                                                         |
| `bench.isolated` | Только сверяется с `/sys/devices/system/cpu/isolated`. **Выставить `isolcpus` из скрипта нельзя** — это параметр загрузки ядра. |
| `cpu`            | Необязателен в любой секции; без него процесс не пиннится.                                                                      |

Артефакты в `results/<ts>/`: `<point>-rx-N.csv`, логи всех процессов, `config.toml`, `env.txt` (cmdline, isolated, governor, no_turbo, git), `summary.csv` со строками `rate,fanout,repeat,receiver,n,drop_rate,p50,p99,p99.9,p99.99`.

### Покрытие флагов бинарей

| бинарь | флаг | в конфиге |
|---|---|---|
| producer | `--rate` | `producer.rate` (свипуемый) |
| | `--type` | `producer.type` |
| | `--slots` | `producer.slots`, общий для всех колец |
| | `--count` | нет — прибит к `0`, останавливается только consumer |
| | `--shm` | нет — `/fanout_ring` |
| sender | `--batch` | `sender.batch` |
| | `--sndbuf` | **нет**, дефолт 8 MiB |
| | `--dst` | нет — собирается из портов получателей |
| | `--from-edge` | нет — всегда включён |
| | `--count`, `--idle-ms` | нет |
| receiver | `--listen` | `receiver.port` |
| | `--batch` | **берётся из `sender.batch`** |
| | `--rcvbuf` | **нет**, дефолт 8 MiB |
| consumer | `--count` | `consumer.count` |
| | `--warmup` | `consumer.warmup` |
| | `--csv` | `receiver.csv`, необязательный, по умолчанию автоимя |
| | `--idle-ms` | нет, дефолт 2000 мс — он же страховка от зависания, своего таймаута у `bench.py` нет |
| | `--from-edge` | нет — всегда включён |

Свипуются только `producer.rate` и `bench.fanout`.

Пробелы, которые стоит закрыть (~10 строк), учитывая, что узкое место — насыщение sender'а:

1. `sndbuf` / `rcvbuf` — прямо влияют на потери при насыщении, сейчас зашиты в дефолт.
2. Отдельный `receiver.batch` — сейчас receiver получает батч sender'а, хотя это разные вещи (`sendmmsg` против `recvmmsg`).
3. Свипуемость `batch` по той же схеме «список = свип».

## Замеры (i7-12650H, loopback, без isolcpus, governor=powersave; count=50000, warmup=5000)

Свип `bench.py`, ядра 2/4 под producer/sender, получатели на 6/10/13, consumer'ы на 8/12/14:

| rate | fanout | drops | p50          | p99          | p99.9      |
|------|--------|-------|--------------|--------------|------------|
| 150k | 1      | 0%    | 2.3 µs       | 3.7 µs       | 13.8 µs    |
| 150k | 2      | 0%    | 2.3 / 4.0 µs | 3.8 / 6.4 µs | 14 / 19 µs |
| 150k | 3      | 0%    | 2.4–6.2 µs   | 132–136 µs   | 201–206 µs |
| 300k | 1      | 0%    | 2.3 µs       | 4.2 µs       | 10.4 µs    |
| 300k | 2      | 0.25% | 2.71 ms      | 3.42 ms      | 3.44 ms    |
| 300k | 3      | 44.4% | 3.49 ms      | 3.70 ms      | 3.78 ms    |

Колено между fanout 1 и 2 при 300k и между 2 и 3 при 150k — то есть потолок sender'а по датаграммам, а не по сообщениям.

Узкое место — sender, не UDP-хоп: receiver'ы публикуют ровно столько, сколько sender отправил, а сам он отстаёт от кольца producer'а (`lapped` в его логе). Потолок ~218k msg/s при трёх dst — один `sendmmsg` на (сообщение × получатель), 264-байтные датаграммы, ~650k pkt/s.

p50 растёт монотонно с номером получателя (2.4 / 4.1 / 6.2 µs при 150k×3) — цена лишнего dst внутри одной пачки `sendmmsg`; последний в списке платит за всех.

Цифры на ноутбуке без `isolcpus` — ориентир по поведению, не по абсолютным значениям. Для сравнимых чисел — `start_with_taskset.sh` (повторы + run-to-run spread) или `repeats` в конфиге.

## Профилирование

Включается секцией `[profile]` в конфиге (`enabled = true`), флагов у скрипта по-прежнему нет.
Артефакты — в `results/<ts>/prof/`, сводка — там же в `index.html`: таблица «точка × бинарник», где рядом
стоят хвост латентности и то, чем в этот момент был занят каждый процесс (IPC, промахи кэша, topdown,
флейм-графы). Код — `scripts/profilers.py` (сбор) и `scripts/profreport.py` (отчёт).

### Почему attach, а не обёртка

Профайлер цепляется к уже запущенному процессу (`perf ... -p <pid>`), а не оборачивает его.
Обёртка `perf record -- ./bin/sender` не годится, проверено:

- убийство `perf` **не убивает потомка** — спиннеры остаются висеть вместе с портами и shm-сегментами;
- `perf stat` по SIGTERM **не пишет вообще ничего** (а `stop()` в `bench.py` шлёт именно SIGTERM).

При attach SIGTERM цели доводит `perf` до штатного завершения с полными артефактами, а `spawn()`/`stop()`
не меняются вовсе. Сам сборщик пинится на `collector_cpu` (по умолчанию 0), чтобы не отъедать такты
у busy-spin процесса: на его ядре остаётся только NMI от сэмплирования.

### Инструменты

| tools | что даёт | привязка |
|---|---|---|
| `stat` | cycles, instructions, IPC, cache-refs/misses, LLC, branch-misses, dTLB, ctx-sw, миграции + topdown | `-p pid` |
| `record` | флейм-граф по циклам | `-p pid` |
| `cache-miss` | флейм-граф по промахам кэша | `-p pid` |
| `shared = ["c2c"]` | false sharing: HITM по строкам кэша | `-C ядра` |
| `shared = ["mem"]` | латентность обращений к памяти, snoop, TLB | `-C ядра` |

PMU (`cpu_core` / `cpu_atom`) выбирается по ядру роли из конфига: на гибридном чипе обобщённые имена
событий разворачиваются в оба PMU и половина строк выходит `<not counted>`, а `perf mem` с обоими PMU
в группе просто падает по ENOENT.

### Два повтора, а не один

`c2c` и `mem` — системный сбор PEBS на тех же ядрах, и он вытесняет per-task события. Замерено на
sender'е при одинаковой нагрузке:

| набор | сэмплов у `record` | multiplexing у `stat` |
|---|---|---|
| только `record` | 3K | — |
| `stat` + `record` | 3K | нет |
| **+ `c2c`** | **6** | 30% |
| всё сразу | 18 | 62% |

Отсюда раздельные `profile.repeats` (профайлеры по ролям) и `profile.shared_repeats` (c2c/mem);
при пересечении `bench.py` ругается на старте.

### Возмущение хвоста

`stat` — счётчики, PMU считает в фоне без прерываний: его можно держать включённым всегда.
`record`/`cache-miss` при `-F 997` дают доли процента накладных, но ложатся **именно в хвост**, а `c2c`/`mem`
заметно тяжелее (в одном прогоне p99 уехал с 6 µs до 490 µs). Поэтому профилированные точки помечены
колонкой `profiled` в `summary.csv`, а в `scripts/latencies.csv` для ноутбука уходит чистая точка.

Оба ключа задаются **номерами** повторов внутри точки (1..`bench.repeats`), а не количеством;
`[]` означает «все повторы». По умолчанию профилируется повтор 1 (по ролям) и повтор 2 (c2c) —
остальные повторы серии чистые, поэтому `bench.repeats` имеет смысл держать хотя бы 3. При
`bench.repeats = 2` чистых повторов не остаётся вовсе, и `export_for_notebook` (`scripts/bench.py`)
по фолбэку отдаёт в `scripts/latencies.csv` профилированную точку.

### Флейм-графы

Нужны два скрипта Brendan Gregg в `scripts/vendor/`, в репозитории их нет:

```
mkdir -p scripts/vendor && cd scripts/vendor
curl -LO https://raw.githubusercontent.com/brendangregg/FlameGraph/master/stackcollapse-perf.pl
curl -LO https://raw.githubusercontent.com/brendangregg/FlameGraph/master/flamegraph.pl
chmod +x *.pl
```

Без них прогон не ломается: `.folded` (готовый вход для `flamegraph.pl`) сохраняется, SVG пропускается
с предупреждением. `-g -fno-omit-frame-pointer` в `Makefile` уже есть, так что стеки честные,
а `setarch -R` держит раскладку кода стабильной между прогонами.

## Не проверено

- `--loss` / `--delay` (netem) в `start_simple.sh`: код есть, прогона не было — нужен sudo.
- `--no-pin` в `start_simple.sh`.
- **VTune из `bench.py` убран.** Причина: attach-сбор не завершается ни по `-command stop`, ни по SIGINT,
  ни по смерти цели (в его `log/` видно `Could not start ContainerMapping collector` — спотыкается о PID
  namespace), а обход этого стоил скрипту отдельного порядка останова и таймаутов на каждой точке.
  Рабочий путь — `perf`; если VTune понадобится, его проще навесить руками на уже запущенный прогон
  (`vtune -collect hotspots -target-pid ...`) и смотреть в `vtune-gui`. `scripts/vtune_setup.sh` остаётся:
  он выставляет `perf_event_paranoid`/`kptr_restrict`/`ptrace_scope`, которые нужны и самому `perf`.
- **Трейспоинты недоступны** (`sched:*`, off-CPU, syscalls): tracefs закрыт, `perf` отвечает
  `can't access trace events`. Нужен root — поэтому off-CPU профилирования в наборе нет.
- `valgrind`/`cachegrind` на машине не установлены.

## Прочее