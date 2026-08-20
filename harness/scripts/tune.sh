#!/usr/bin/env bash
# Prepare (and restore) the host for latency benchmarking.
#
# Two things move the tail, both of them host state rather than anything the
# benchmark itself can do:
#   1. a fixed clock -- powersave + turbo let the cores roam between 400 MHz and
#      4.7 GHz, so the same code is timed at different speeds. performance
#      governor + no_turbo pins them at base frequency.
#   2. quiet neighbours -- a hyper-thread sibling shares execution units with the
#      bench core, and any core on the package shares the last-level cache and
#      the power budget. Offline cores cannot interfere.
#
#   ./tune.sh                  # apply, needs sudo; once per boot
#   ./tune.sh --untune         # restore whatever was saved
#   ./tune.sh --offline 3,5    # take exactly these offline (and tune the clock)
#   ./tune.sh --online 3,5     # put these back online, leaving the clock tuned
#
# OFFLINE is a plain list of CPU ids: on this box cpu3/cpu5 are the HT siblings
# of the bench cores 2/4, cpu7/cpu9 are the noisy neighbours on the package.
# `lscpu -e` shows which id is which.
#
# Not covered here (needs a reboot): isolcpus / nohz_full, so the periodic tick
# still shows up in p99.99.
set -euo pipefail
SELF=$(readlink -f "$0")
cd "$(dirname "$SELF")/.."

OFFLINE=3,5,7,9
STATE=scripts/.bench_host_state

usage() { awk 'NR > 1 && /^#/ {sub(/^# ?/, ""); print; next} NR > 1 {exit}' "$SELF"; }

tune() {
  [[ -f $STATE ]] && { echo "already tuned ($STATE exists; --untune first)"; exit 1; }
  { echo "governor $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
    echo "no_turbo $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo -)"
    echo "offline $OFFLINE"; } > "$STATE"

  sudo bash -c '
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$g"; done
    echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || true
    for c in '"${OFFLINE//,/ }"'; do echo 0 > "/sys/devices/system/cpu/cpu$c/online"; done'
  echo "tuned: performance governor, turbo off, cpu$OFFLINE offline"
  [[ $(cat /sys/class/power_supply/ACAD/online 2>/dev/null || echo 1) == 1 ]] ||
    echo "note: on battery the power limit tracks the charge level, so the clock still drifts"
}

online() {  # bring cores back without undoing the clock settings
  local cpus=$1 saved rest
  sudo bash -c '
    for c in '"${cpus//,/ }"'; do echo 1 > "/sys/devices/system/cpu/cpu$c/online"; done'
  if [[ -f $STATE ]]; then
    # Drop them from the saved list, or --untune would "restore" cores that are
    # already up and the state file would claim they are still offline.
    saved=$(awk '/^(offline|siblings)/ {print $2}' "$STATE")
    rest=$(tr ',' '\n' <<<"$saved" | grep -vxF -f <(tr ',' '\n' <<<"$cpus") | paste -sd, - || true)
    sed -i "s/^\(offline\|siblings\) .*/offline $rest/" "$STATE"
    echo "cpu$cpus online; still offline: ${rest:+cpu}${rest:-none}"
  else
    echo "cpu$cpus online"
  fi
}

untune() {
  [[ -f $STATE ]] || { echo "nothing to undo (no $STATE)"; exit 1; }
  local gov no_turbo cpus
  gov=$(awk '/^governor/ {print $2}' "$STATE")
  no_turbo=$(awk '/^no_turbo/ {print $2}' "$STATE")
  # `siblings` is what older state files called the list; still read it.
  cpus=$(awk '/^(offline|siblings)/ {print $2}' "$STATE" | tr ',' ' ')
  sudo bash -c '
    for c in '"$cpus"'; do echo 1 > "/sys/devices/system/cpu/cpu$c/online"; done
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo '"$gov"' > "$g"; done
    [ "'"$no_turbo"'" = - ] || echo '"$no_turbo"' > /sys/devices/system/cpu/intel_pstate/no_turbo'
  rm -f "$STATE"
  echo "restored: governor=$gov, no_turbo=$no_turbo, cpu${cpus// /,} online"
}

case ${1-} in
  ""|--tune) tune ;;
  --untune) untune ;;
  --offline) OFFLINE=${2?--offline needs a core list}; tune ;;
  --online) online "${2?--online needs a core list}" ;;
  -h|--help) usage ;;
  *) usage; exit 1 ;;
esac
