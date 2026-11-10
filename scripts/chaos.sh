#!/usr/bin/env bash
# Kill and restart random shard processes (primaries and replicas) while loadgen runs.
# Passes if the coordinator and loadgen survive and no sanitizer report appears.
# Usage: scripts/chaos.sh
# Env: BIN (build/kretrieve; point at a TSan/ASan build to hunt races), DURATION (s, 30),
#      QPS (200), INTERVAL (s between kills, 2), DOWN (s a victim stays dead, 1),
#      BUDGET_US (200000), SEED (random).
set -euo pipefail

cd "$(dirname "$0")/.."
bin=${BIN:-build/kretrieve}
duration=${DURATION:-30}
qps=${QPS:-200}
interval=${INTERVAL:-2}
down=${DOWN:-1}
budget_us=${BUDGET_US:-200000}
data=bench/data
[[ -x $bin ]] || { echo "build $bin first" >&2; exit 1; }
[[ -f $data/corpus.tsv ]] || python3 scripts/gen_corpus.py --out "$data"
[[ -d $data/prepared ]] || "$bin" prepare --input "$data/corpus.tsv" --output "$data/prepared" --shards 3
RANDOM=${SEED:-$RANDOM}

run=$(mktemp -d)
declare -A pid port
coord_pid=
cleanup() {
  kill "${pid[@]}" "$coord_pid" 2>/dev/null || true
  wait 2>/dev/null || true
  rm -rf -- "$run"
}
trap cleanup EXIT

wait_listening() { # <name> <pid>
  for _ in $(seq 600); do
    grep -q '^listening on ' "$run/$1.out" && return 0
    kill -0 "$2" 2>/dev/null || { echo "$1 exited at startup" >&2; exit 1; }
    sleep 0.1
  done
  echo "$1 did not start" >&2
  exit 1
}

# start_shard <name> <partition> [port]: (re)start a shard; records pid[name], port[name].
start_shard() {
  local name=$1 s=$2 p=${3:-0}
  : >"$run/$name.out"
  "$bin" shard --stats "$data/prepared/global_stats.bin" \
    --partition "$data/prepared/shard-$s.tsv" --port "$p" >>"$run/$name.out" 2>>"$run/$name.err" &
  pid[$name]=$!
  wait_listening "$name" "${pid[$name]}"
  port[$name]=$(sed -n 's/^listening on .*:\([0-9][0-9]*\)$/\1/p' "$run/$name.out")
}

names=()
prim=() repl=()
for s in 0 1 2; do
  start_shard "p$s" "$s"
  start_shard "r$s" "$s"
  names+=("p$s" "r$s")
  prim+=("127.0.0.1:${port[p$s]}")
  repl+=("127.0.0.1:${port[r$s]}")
done
join() { local IFS=,; echo "$*"; }

"$bin" coordinator --port 0 --workers 8 --max-queue 16 \
  --shards "$(join "${prim[@]}")" --replicas "$(join "${repl[@]}")" \
  >"$run/coord.out" 2>"$run/coord.err" &
coord_pid=$!
wait_listening coord "$coord_pid"
cport=$(sed -n 's/^listening on .*:\([0-9][0-9]*\)$/\1/p' "$run/coord.out")

echo "chaos: ${duration}s at ${qps} qps, kill every ${interval}s (seed ${SEED:-random})"
"$bin" loadgen --port "$cport" --qps "$qps" --duration "$duration" \
  --queries "$data/queries.txt" --budget-us "$budget_us" --connections 48 \
  --out "$run/load.csv" >"$run/load.out" 2>&1 &
load_pid=$!

kills=0
end=$((SECONDS + duration))
while ((SECONDS + interval + down < end)); do
  sleep "$interval"
  victim=${names[RANDOM % ${#names[@]}]}
  old_port=${port[$victim]}
  kill -9 "${pid[$victim]}" 2>/dev/null || true
  wait "${pid[$victim]}" 2>/dev/null || true
  kills=$((kills + 1))
  echo "  killed $victim (port $old_port)"
  sleep "$down"
  start_shard "$victim" "${victim:1}" "$old_port"
done

status=0
wait "$load_pid" || { echo "FAIL: loadgen exited non-zero" >&2; status=1; }
kill -0 "$coord_pid" 2>/dev/null || { echo "FAIL: coordinator died" >&2; status=1; }
if grep -lE 'ERROR: (Address|Thread)Sanitizer|runtime error:|WARNING: ThreadSanitizer' \
  "$run"/*.err "$run"/*.out >/dev/null 2>&1; then
  echo "FAIL: sanitizer report:" >&2
  grep -hE -A15 'Sanitizer|runtime error:' "$run"/*.err "$run"/*.out | head -60 >&2
  status=1
fi
sed 's/^loadgen: //' "$run/load.out"
echo "failovers: $(grep -c '^failover ' "$run/coord.err" || true), kills: $kills"
((status)) && exit 1
echo "chaos: PASS"
