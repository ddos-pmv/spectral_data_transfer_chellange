#!/usr/bin/env bash
# Build the harness and run one producer/consumer pair pinned to isolated cores.
set -e

cd "$(dirname "$0")/.."
make

taskset -c 2 ./bin/producer --slots 1024 --count 1000000 --rate 300000 --type mixed &
producer_pid=$!

sleep 0.02
taskset -c 4 ./bin/consumer --slots 1024 --from-edge --csv scripts/latencies.csv

kill "$producer_pid" 2>/dev/null || true
