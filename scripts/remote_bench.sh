#!/usr/bin/env bash
# Cloud counterpart of scripts/bench.sh: restart the cluster with the given shard flags, then
# sweep QPS from the control VM and copy the CSVs to bench/cloud/results/.
# Usage: scripts/remote_bench.sh <label> [shard flags, e.g. --latency-ms 20-100 --latency-prob 0.1]
# Env: QPS ("100 250 500 1000"), DURATION (10), BUDGET_US (200000), HEDGE_MS (adds replicas and
#      hedging), plus the scripts/cloud_cluster.sh variables. Then: scripts/plot.py bench/cloud
# Loadgen shares the control VM's 2 vCPUs with the coordinator, so read the top rates with that
# in mind.
set -euo pipefail
label=${1:?usage: scripts/remote_bench.sh <label> [shard flags...]}
shift
cd "$(dirname "$0")/.."
source scripts/cloud_lib.sh
init_cluster
qps_list=${QPS:-100 250 500 1000}
duration=${DURATION:-10}
budget_us=${BUDGET_US:-200000}
out=bench/cloud/results
mkdir -p "$out"

scripts/cloud_cluster.sh up "$@"
rsh control "rm -f results/$label-*.csv"

echo "warm-up"
run_loadgen warmup.csv 100 5 "$budget_us" >/dev/null 2>&1 || true
for qps in $qps_list; do
  printf '%s qps=%s: ' "$label" "$qps"
  run_loadgen "$label-$qps.csv" "$qps" "$duration" "$budget_us" 2>&1 | sed 's/^loadgen: //'
done

rsh control "cd results && tar -cf - $label-*.csv" | tar -C "$out" -xf -

# Record what produced these numbers next to them.
{
  echo "date $(date -u +%FT%TZ)"
  echo "label $label"
  echo "shard_flags $*"
  echo "hedge_ms ${HEDGE_MS:-off}"
  echo "shards $(tfo -raw shards_arg)"
  echo "replicas $(tfo -raw replicas_arg)"
  rsh control 'curl -s -H Metadata:true "http://169.254.169.254/metadata/instance/compute?api-version=2021-02-01&format=json"' \
    | python3 -c 'import json, sys
d = json.load(sys.stdin)
print("vm_size", d["vmSize"], "location", d["location"], "zone", d["zone"])'
  rsh control "echo kernel \$(uname -r); docker --version; docker image inspect -f 'image {{.Id}}' $image"
} >"$out/$label.meta"
echo "results in $out/ (summarize: scripts/plot.py bench/cloud)"
