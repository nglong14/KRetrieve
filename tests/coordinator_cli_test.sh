#!/usr/bin/env bash
# End-to-end: prepare 3 shards -> 3 shard processes + coordinator -> query matches 1 shard.
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

# Day 3: a dead shard fails the whole query (day 4 returns partial results).
kill -TERM "${pids[1]}"
wait "${pids[1]}" || fail "shard 1 did not exit cleanly"
rc=0
"$bin" query --port "$coord" --query cat >/dev/null 2>&1 || rc=$?
[[ $rc == 1 ]] || fail "expected query to fail with a dead shard (got $rc)"

kill -TERM "${pids[3]}"
wait "${pids[3]}" || fail "coordinator did not exit cleanly on SIGTERM"
echo "coordinator CLI OK"
