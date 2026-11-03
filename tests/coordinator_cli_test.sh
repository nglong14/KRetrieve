#!/usr/bin/env bash
# End-to-end: prepare 3 shards -> 3 shard processes + coordinator -> query matches 1 shard,
# then kill a shard, and slow one down, and expect partial results.
# Usage: coordinator_cli_test.sh <kretrieve-binary> <scratch-parent-dir>
set -euo pipefail

bin=$1
root=$(mktemp -d "$2/coordinator-cli.XXXXXX")
pids=()
cleanup() {
  for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  rm -rf -- "$root"
}
trap cleanup EXIT
fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# start <name> <role> [args...]: run in background; sets $port once listening.
start() {
  local name=$1
  shift
  "$bin" "$@" --port 0 >"$root/$name.out" &
  pids+=($!)
  for _ in $(seq 50); do
    grep -q '^listening on ' "$root/$name.out" && break
    sleep 0.1
  done
  port=$(sed -n 's/^listening on .*:\([0-9][0-9]*\)$/\1/p' "$root/$name.out")
  [[ -n $port ]] || fail "$name did not report a port"
}

printf '10\tcat cat dog\n20\tdog bird\n30\tcat bird bird\n' >"$root/corpus.tsv"
"$bin" prepare --input "$root/corpus.tsv" --output "$root/prepared" --shards 3 >/dev/null

ports=()
for s in 0 1 2; do
  start "shard$s" shard --stats "$root/prepared/global_stats.bin" \
    --partition "$root/prepared/shard-$s.tsv"
  ports+=("$port")
done
start coordinator coordinator --workers 2 \
  --shards "127.0.0.1:${ports[0]},127.0.0.1:${ports[1]},127.0.0.1:${ports[2]}"
coord=$port

# Same expected output as the single-shard test: global stats make sharding invisible.
expected=$'1\t10\t1.071445295349\n2\t20\t0.523548346502\n3\t30\t0.447138587823'
actual=$("$bin" query --port "$coord" --k 10 --query "cat dog")
[[ $actual == "$expected" ]] || fail "unexpected hits:"$'\n'"$actual"

# A dead shard is left out: the surviving shards' hits come back, flagged incomplete.
# prepare partitions by id % 3, so shard 1 holds doc 10.
kill -TERM "${pids[1]}"
wait "${pids[1]}" || fail "shard 1 did not exit cleanly"
actual=$("$bin" query --port "$coord" --k 10 --query "cat dog" --budget-us 500000 \
  2>"$root/query.err") || fail "partial query failed"
expected=$'1\t20\t0.523548346502\n2\t30\t0.447138587823'
[[ $actual == "$expected" ]] || fail "unexpected partial hits:"$'\n'"$actual"
grep -qx 'incomplete: missing shards 1' "$root/query.err" ||
  fail "missing incomplete flag: $(cat "$root/query.err")"

# A slow shard (--latency-ms) misses a 100 ms budget: same partial result as a dead one.
start slow shard --stats "$root/prepared/global_stats.bin" \
  --partition "$root/prepared/shard-1.tsv" --latency-ms 300
start coordinator2 coordinator --workers 2 \
  --shards "127.0.0.1:${ports[0]},127.0.0.1:$port,127.0.0.1:${ports[2]}"
actual=$("$bin" query --port "$port" --k 10 --query "cat dog" --budget-us 100000 \
  2>"$root/slow.err") || fail "slow-shard query failed"
[[ $actual == "$expected" ]] || fail "unexpected slow-shard hits:"$'\n'"$actual"
grep -qx 'incomplete: missing shards 1' "$root/slow.err" ||
  fail "missing incomplete flag for slow shard: $(cat "$root/slow.err")"
# Bad latency flags are usage errors (exit 2), rejected before any file is read.
expect_usage() {
  local rc=0
  "$bin" "$@" >/dev/null 2>&1 || rc=$?
  [[ $rc == 2 ]] || fail "expected usage error from: $* (got $rc)"
}
expect_usage shard --stats x --partition y --port 0 --latency-ms 5-1
expect_usage shard --stats x --partition y --port 0 --latency-ms 5 --latency-prob 2
expect_usage shard --stats x --partition y --port 0 --latency-prob 0.5

kill -TERM "${pids[3]}"
wait "${pids[3]}" || fail "coordinator did not exit cleanly on SIGTERM"
echo "coordinator CLI OK"
