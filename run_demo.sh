#!/usr/bin/env bash
# One-command demo: kill -9 a shard mid-service, first without and then with a replica.
# Usage: ./run_demo.sh   (env: BIN=build/kretrieve, PAUSE=1 seconds between steps)
set -euo pipefail
cd "$(dirname "$0")"
bin=${BIN:-build/kretrieve}
pause=${PAUSE:-1}
[[ -x $bin ]] || { cmake -S . -B build && cmake --build build -j; }

dir=$(mktemp -d)
pids=()
trap 'kill "${pids[@]}" 2>/dev/null || true; rm -rf -- "$dir"' EXIT

printf '%s\n' $'1\tcat cat dog' $'2\tdog bird' $'3\tcat bird bird' $'4\tcat fish' \
  $'5\tdog dog cat' $'6\tbird fish' >"$dir/corpus.tsv"
"$bin" prepare --input "$dir/corpus.tsv" --output "$dir/p" --shards 3 >/dev/null

# launch <name> <args...>: start in background, wait for "listening on", set $port and $pid.
launch() {
  local name=$1
  shift
  "$bin" "$@" --port 0 >"$dir/$name.out" 2>"$dir/$name.err" &
  pid=$!
  pids+=("$pid")
  disown "$pid"
  until grep -q '^listening on ' "$dir/$name.out"; do sleep 0.1; done
  port=$(sed -n 's/^listening on .*:\([0-9][0-9]*\)$/\1/p' "$dir/$name.out")
}
shard() { launch "$1" shard --stats "$dir/p/global_stats.bin" --partition "$dir/p/shard-$2.tsv"; }
say() { printf '\n\033[1m# %s\033[0m\n' "$*"; sleep "$pause"; }
query() { echo '$ kretrieve query --query "cat dog"'; "$bin" query --port "$1" --k 3 --query "cat dog" 2>&1 || true; sleep "$pause"; }

prim=() repl=() ppid=()
for s in 0 1 2; do
  shard "a$s" "$s"; prim+=("127.0.0.1:$port"); ppid+=("$pid")
  shard "b$s" "$s"; repl+=("127.0.0.1:$port")
done
join() { local IFS=,; echo "$*"; }

say "Act 1: 3 shards, no replicas"
launch coord1 coordinator --shards "$(join "${prim[@]}")"
c1=$port
query "$c1"
say "kill -9 shard 1"
kill -9 "${ppid[1]}"; sleep 0.3
query "$c1"
echo "^ still answers: partial result, shard 1 flagged missing"; sleep "$pause"

say "Act 2: same cluster plus a replica per shard"
launch coord2 coordinator --shards "$(join "${prim[@]}")" --replicas "$(join "${repl[@]}")"
c2=$port
query "$c2"
echo "^ shard 1's primary is already dead: the replica answered, result is complete"
grep '^failover' "$dir/coord2.err" | head -1 || true
