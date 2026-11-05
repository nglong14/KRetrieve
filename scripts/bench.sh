#!/usr/bin/env bash
# Run a QPS sweep against 3 shards + a coordinator and write one CSV per rate.
# Usage: scripts/bench.sh <label> [extra shard flags, e.g. --latency-ms 20-100 --latency-prob 0.1]
# Env: BIN (default build/kretrieve), QPS ("100 250 500 1000 2000 4000"), DURATION (s, 10),
#      BUDGET_US (200000), HEDGE_MS (unset; set it to add one replica per shard, with the
#      same shard flags, and hedge to it after that many ms).
set -euo pipefail

label=${1:?usage: scripts/bench.sh <label> [shard flags...]}
shift
cd "$(dirname "$0")/.."
bin=${BIN:-build/kretrieve}
qps_list=${QPS:-100 250 500 1000 2000 4000}
duration=${DURATION:-10}
budget_us=${BUDGET_US:-200000}
data=bench/data
results=bench/results
[[ -x $bin ]] || { echo "build $bin first" >&2; exit 1; }
mkdir -p "$results"

[[ -f $data/corpus.tsv ]] || python3 scripts/gen_corpus.py --out "$data"
[[ -d $data/prepared ]] || "$bin" prepare --input "$data/corpus.tsv" --output "$data/prepared" --shards 3

run=$(mktemp -d)
pids=()
cleanup() {
  for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  wait 2>/dev/null || true
  rm -rf -- "$run"
}
trap cleanup EXIT

# start <name> <role> [args...]: run in background; sets $port once listening.
start() {
  local name=$1
  shift
  "$bin" "$@" --port 0 >"$run/$name.out" &
  pids+=($!)
  for _ in $(seq 600); do # index builds take a few seconds
    grep -q '^listening on ' "$run/$name.out" && break
    kill -0 "${pids[-1]}" 2>/dev/null || { echo "$name exited" >&2; exit 1; }
    sleep 0.1
  done
  port=$(sed -n 's/^listening on .*:\([0-9][0-9]*\)$/\1/p' "$run/$name.out")
  [[ -n $port ]] || { echo "$name did not start" >&2; exit 1; }
}

# start_shards <name> [shard flags...]: start shards 0-2; sets $addrs to "host:port,...".
start_shards() {
  local name=$1 list=()
  shift
  for s in 0 1 2; do
    start "$name$s" shard --stats "$data/prepared/global_stats.bin" \
      --partition "$data/prepared/shard-$s.tsv" "$@"
    list+=("127.0.0.1:$port")
  done
  addrs=$(IFS=,; echo "${list[*]}")
}

start_shards shard "$@"
coord_flags=(--shards "$addrs")
if [[ -n ${HEDGE_MS:-} ]]; then
  start_shards replica "$@"
  coord_flags+=(--replicas "$addrs" --hedge-ms "$HEDGE_MS")
fi
# Small worker pool and queue so the top rates show Overloaded rather than unbounded waits.
start coordinator coordinator --workers 8 --max-queue 16 "${coord_flags[@]}"

for qps in $qps_list; do
  printf '%s qps=%s: ' "$label" "$qps"
  "$bin" loadgen --port "$port" --qps "$qps" --duration "$duration" \
    --queries "$data/queries.txt" --budget-us "$budget_us" --connections 48 \
    --out "$results/$label-$qps.csv" 2>&1 | sed 's/^loadgen: //'
done
