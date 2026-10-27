#!/usr/bin/env bash
# End-to-end: prepare -> start shard on an ephemeral port -> query over TCP -> SIGTERM.
# Usage: shard_cli_test.sh <kretrieve-binary> <scratch-parent-dir>
set -euo pipefail

bin=$1
root=$(mktemp -d "$2/shard-cli.XXXXXX")
pid=
cleanup() {
  if [[ -n $pid ]]; then kill "$pid" 2>/dev/null || true; fi
  rm -rf -- "$root"
}
trap cleanup EXIT
fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# expect_fail <exit-code> <role> [args...]: fails cleanly with a "<role>:" message.
expect_fail() {
  local code=$1 rc=0
  shift
  "$bin" "$@" >/dev/null 2>"$root/err" || rc=$?
  [[ $rc == "$code" ]] && grep -q "^$1:" "$root/err" || fail "expected exit $code: $* (got $rc)"
}

printf '10\tcat cat dog\n20\tdog bird\n30\tcat bird bird\n' >"$root/corpus.tsv"
"$bin" prepare --input "$root/corpus.tsv" --output "$root/prepared" --shards 1 >/dev/null
stats=$root/prepared/global_stats.bin
partition=$root/prepared/shard-0.tsv

expect_fail 2 shard --stats "$stats" --partition "$partition"
expect_fail 2 shard --stats "$stats" --partition "$partition" --port 65536
expect_fail 2 shard --stats "$stats" --partition "$partition" --port 0 --max-connections 0
expect_fail 2 shard --stats "$stats" --partition "$partition" --port 0 --workers 0
expect_fail 2 shard --stats "$stats" --partition "$partition" --port 0 --workers -1
expect_fail 1 shard --stats "$root/missing.bin" --partition "$partition" --port 0
expect_fail 2 query --query cat
expect_fail 1 query --port 1 --query cat  # nothing listening on port 1

"$bin" shard --stats "$stats" --partition "$partition" --port 0 --workers 2 >"$root/out" &
pid=$!
for _ in $(seq 50); do
  grep -q '^listening on ' "$root/out" && break
  sleep 0.1
done
port=$(sed -n 's/^listening on .*:\([0-9][0-9]*\)$/\1/p' "$root/out")
[[ -n $port ]] || fail "shard did not report a listening port"

expected=$'1\t10\t1.071445295349\n2\t20\t0.523548346502\n3\t30\t0.447138587823'
actual=$("$bin" query --port "$port" --k 10 --query "cat dog")
[[ $actual == "$expected" ]] || fail "unexpected hits:"$'\n'"$actual"
expect_fail 1 query --port "$port" --k 100001 --query cat  # shard answers BadRequest

kill -TERM "$pid"
wait "$pid" || fail "shard did not exit cleanly on SIGTERM"
pid=
echo "shard CLI OK"
