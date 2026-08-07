#!/usr/bin/env bash
# Run the fan-out benchmark N times on isolated cores and report the median and
# the run-to-run spread of the latency percentiles.
#
# Why this is more than `taskset`: pinning was already here, and it only
# stabilised p50 (spread 2.7% vs 187% at p99.9). What actually moves the tail,
# in order:
#   1. a fixed sample -- the consumer records exactly --count messages after
#      discarding --warmup, so every repeat measures the same work. The old
#      `--from-edge` + `sleep 0.02` caught a different number of messages each
#      time, i.e. every run was a different experiment.
#   2. a fixed clock -- powersave + turbo let these cores roam between 400 MHz
#      and 4.7 GHz. `--tune` pins them at base frequency.
#   3. idle hyper-thread siblings -- cpu3/cpu5 share execution units with the
#      bench cores 2/4. `--tune` takes them offline.
#   4. repeats -- a single run's p99.9 is a sample, not a number.
#
#   ./start_with_taskset.sh --tune       # once per boot, needs sudo (--untune undoes it)
#   ./start_with_taskset.sh --repeats 7
#
# Not fixed here (needs a reboot): isolcpus / nohz_full, so the periodic tick
# still shows up in p99.99. On the AWS bench host that is worth adding.
set -euo pipefail
SELF=$(readlink -f "$0")
cd "$(dirname "$SELF")/.."

RATE=300000
COUNT=500000        # recorded samples per run
WARMUP=50000        # messages consumed and thrown away before recording
REPEATS=5
PCPU=2
CCPU=4
SLOTS=1024
TYPE=mixed
SHM=/fanout_ring
STATE=scripts/.bench_host_state
RESULTS=results

usage() {
  awk 'NR > 1 && /^#/ {sub(/^# ?/, ""); print; next} NR > 1 {exit}' "$SELF"
  cat <<'EOF'

Options: --rate R --count N --warmup N --repeats N --type T --slots N
         --cpus P,C   producer,consumer cores (default 2,4)
         --tune       governor=performance, turbo off, HT siblings offline
         --untune     undo --tune
EOF
}

expand() { local p; for p in ${1//,/ }; do
  if [[ $p == *-* ]]; then seq "${p%-*}" "${p#*-}"; else echo "$p"; fi; done; }

siblings() {  # hyper-thread peers of the bench cores
  local c; for c in "$PCPU" "$CCPU"; do
    expand "$(cat "/sys/devices/system/cpu/cpu$c/topology/thread_siblings_list")"
  done | sort -un | grep -vx -e "$PCPU" -e "$CCPU" || true
}

tune() {
  [[ -f $STATE ]] && { echo "already tuned ($STATE exists)"; exit 1; }
  { echo "governor $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
    echo "no_turbo $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo -)"
    echo "siblings $(siblings | paste -sd, -)"; } > "$STATE"

  sudo bash -c '
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$g"; done
    echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || true
    for c in '"$(siblings | paste -sd' ' -)"'; do echo 0 > "/sys/devices/system/cpu/cpu$c/online"; done'
  echo "tuned: performance governor, turbo off, cpu$(siblings | paste -sd, -) offline"
  [[ $(cat /sys/class/power_supply/ACAD/online 2>/dev/null || echo 1) == 1 ]] ||
    echo "note: on battery the power limit tracks the charge level, so the clock still drifts"
}

untune() {
  [[ -f $STATE ]] || { echo "nothing to undo (no $STATE)"; exit 1; }
  local gov no_turbo sibs
  gov=$(awk '/^governor/ {print $2}' "$STATE")
  no_turbo=$(awk '/^no_turbo/ {print $2}' "$STATE")
  sibs=$(awk '/^siblings/ {print $2}' "$STATE" | tr ',' ' ')
  sudo bash -c '
    for c in '"$sibs"'; do echo 1 > "/sys/devices/system/cpu/cpu$c/online"; done
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo '"$gov"' > "$g"; done
    [ "'"$no_turbo"'" = - ] || echo '"$no_turbo"' > /sys/devices/system/cpu/intel_pstate/no_turbo'
  rm -f "$STATE"
  echo "restored: governor=$gov, no_turbo=$no_turbo, cpu${sibs// /,} online"
}

while [[ $# -gt 0 ]]; do
  case $1 in
    --rate) RATE=$2; shift 2 ;;
    --count) COUNT=$2; shift 2 ;;
    --warmup) WARMUP=$2; shift 2 ;;
    --repeats) REPEATS=$2; shift 2 ;;
    --type) TYPE=$2; shift 2 ;;
    --slots) SLOTS=$2; shift 2 ;;
    --cpus) PCPU=${2%,*}; CCPU=${2#*,}; shift 2 ;;
    --tune) tune; exit 0 ;;
    --untune) untune; exit 0 ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 1 ;;
  esac
done

cleanup() {
  pkill -u "$(id -u)" -x producer 2>/dev/null || true
  pkill -u "$(id -u)" -x consumer 2>/dev/null || true
  rm -f "/dev/shm/${SHM#/}"
}
trap cleanup EXIT INT TERM

# Forced rebuild with pinned flags: otherwise you measure whatever happened to
# be in bin/ -- including a CLion Debug build.
make -B CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Iinclude" >/dev/null

# taskset pins the thread to one core so the scheduler cannot migrate it;
# setarch -R disables ASLR so code layout is the same every run; timeout is the
# guard against a busy-spinner that never exits.
#
# Deliberately no SCHED_FIFO here: it would also need RT bandwidth throttling
# turned off (kernel.sched_rt_runtime_us), or the kernel stalls a 100%-CPU RT
# spinner for 50 ms every second and stamps that straight into the tail.
TMO=$(( (COUNT + WARMUP) / RATE + 30 ))
pin() { local cpu=$1; shift; timeout -k 5 "$TMO" taskset -c "$cpu" setarch -R "$@"; }

RUNDIR="$RESULTS/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUNDIR"
echo "$REPEATS run(s): $COUNT samples after $WARMUP warm-up, $RATE msg/s, $TYPE, cores $PCPU/$CCPU"

for ((i = 1; i <= REPEATS; ++i)); do
  cleanup
  # The producer runs unbounded and is stopped once the consumer has its
  # samples, so a slow start can never leave the consumer short.
  pin "$PCPU" ./bin/producer --shm "$SHM" --slots "$SLOTS" --count 0 \
      --rate "$RATE" --type "$TYPE" >/dev/null 2>&1 &
  producer=$!
  sleep 0.2  # let the producer create and initialise the segment
  rc=0
  pin "$CCPU" ./bin/consumer --shm "$SHM" --slots "$SLOTS" --count "$COUNT" \
      --warmup "$WARMUP" --from-edge \
      --csv "$RUNDIR/run-$i.csv" >"$RUNDIR/run-$i.txt" 2>&1 || rc=$?
  pkill -u "$(id -u)" -x producer 2>/dev/null || true
  wait "$producer" 2>/dev/null || true
  # rc 3 means the consumer recorded fewer than --count samples: not the run
  # that was asked for, so it must not end up in the median.
  [[ $rc == 0 ]] && echo "  run $i ok" || { echo "  run $i FAILED (rc=$rc)"; rm -f "$RUNDIR/run-$i.csv"; }
  sleep 2  # let the package cool between runs
done

python3 - "$RUNDIR" scripts/latencies.csv <<'PY'
import glob, os, shutil, statistics, sys

rundir, notebook_csv = sys.argv[1], sys.argv[2]
PCTS = [("p50", .50), ("p99", .99), ("p99.9", .999), ("p99.99", .9999)]

def pct(v, p):  # nearest-rank, same definition as metrics.h
    n = len(v); r = int(p * n)
    if r < p * n: r += 1
    return v[max(1, min(r, n)) - 1]

runs = []
for path in sorted(glob.glob(os.path.join(rundir, "run-*.csv"))):
    with open(path) as f:
        next(f, None)
        v = sorted(int(line.split(",")[1]) for line in f if line.strip())
    if v:
        runs.append((os.path.basename(path), v))

if not runs:
    sys.exit("no usable runs")

print(f"\n{'run':<12}{'n':>9}" + "".join(f"{n:>11}" for n, _ in PCTS) + f"{'max':>11}")
for name, v in runs:
    print(f"{name:<12}{len(v):>9,}" + "".join(f"{pct(v, p):>11,}" for _, p in PCTS) + f"{v[-1]:>11,}")

print(f"\n{'':<12}{'median':>20}{'min':>11}{'max':>11}{'spread':>10}")
for name, p in PCTS:
    vals = [pct(v, p) for _, v in runs]
    med, lo, hi = statistics.median(vals), min(vals), max(vals)
    # The spread is the resolution of this bench: a change smaller than it has
    # not been measured.
    print(f"{name:<12}{med:>17,.0f} ns{lo:>11,}{hi:>11,}{(hi - lo) / med * 100:>9.1f}%")

# The notebook reads one CSV; give it the representative run, not the last one.
median_run = sorted(runs, key=lambda r: pct(r[1], .99))[len(runs) // 2][0]
shutil.copyfile(os.path.join(rundir, median_run), notebook_csv)
print(f"\n{len(runs)} usable run(s) in {rundir}; {median_run} -> {notebook_csv}")
PY
