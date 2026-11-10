#!/usr/bin/env bash
# Cloud counterpart of scripts/chaos.sh: kill things while loadgen runs on the control VM.
# Usage: scripts/remote_chaos.sh
# Env: MODE=container (default): `docker kill` a random shard or replica, restart it after DOWN s.
#      MODE=vm: `az vm deallocate` a random shard VM (primaries and replicas on it die together),
#               then `az vm start` it after DOWN s. Needs `az login`.
#      DURATION (container 60 / vm 600), QPS (200), INTERVAL (5 / 150), DOWN (1 / 90),
#      BUDGET_US (200000), SEED (random), RG (kretrieve-rg).
# Passes if loadgen exits 0 and the coordinator container is still running. The control VM is
# never killed. If interrupted, run scripts/cloud_cluster.sh status and restart what is down.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/cloud_lib.sh
init_cluster
mode=${MODE:-container}
case $mode in
container) duration=${DURATION:-60} interval=${INTERVAL:-5} down=${DOWN:-1} restart=no ;;
vm)
  duration=${DURATION:-600} interval=${INTERVAL:-150} down=${DOWN:-90} restart=unless-stopped
  command -v az >/dev/null || die "az CLI needed for MODE=vm"
  ;;
*) die "MODE must be container or vm" ;;
esac
qps=${QPS:-200}
budget_us=${BUDGET_US:-200000}
rg=${RG:-kretrieve-rg}
RANDOM=${SEED:-$RANDOM}

# Replicas are required; restart policy lets containers come back when a whole VM restarts.
WITH_REPLICAS=1 RESTART=$restart scripts/cloud_cluster.sh up

# Victims: "<vm> <container>" for container mode, "<vm>" for vm mode.
victims=()
if [[ $mode == vm ]]; then
  victims=("${shard_vms[@]}")
else
  for i in "${!shard_addrs[@]}"; do
    victims+=("$(vm_of "${shard_addrs[$i]%:*}") kr-p$i" "$(vm_of "${replica_addrs[$i]%:*}") kr-r$i")
  done
fi

load_out=$(mktemp)
trap 'rm -f -- "$load_out"' EXIT
echo "chaos($mode): ${duration}s at ${qps} qps, kill every ${interval}s, down ${down}s"
run_loadgen chaos.csv "$qps" "$duration" "$budget_us" >"$load_out" 2>&1 &
load_pid=$!

kills=0
end=$((SECONDS + duration))
while ((SECONDS + interval + down < end)); do
  sleep "$interval"
  victim=${victims[RANDOM % ${#victims[@]}]}
  kills=$((kills + 1))
  echo "  kill: $victim"
  if [[ $mode == vm ]]; then
    az vm deallocate -g "$rg" -n "kretrieve-$victim" --no-wait
    sleep "$down"
    az vm start -g "$rg" -n "kretrieve-$victim" --no-wait
  else
    read -r vm name <<<"$victim"
    rsh "$vm" "docker kill $name" >/dev/null
    sleep "$down"
    rsh "$vm" "docker start $name" >/dev/null
  fi
done

status=0
wait "$load_pid" || { echo "FAIL: loadgen exited non-zero" >&2; status=1; }
running=$(rsh control "docker inspect -f '{{.State.Running}}' kr-coord" || true)
[[ $running == true ]] || { echo "FAIL: coordinator not running" >&2; status=1; }
sed 's/^loadgen: //' "$load_out"
failovers=$(rsh control "docker logs kr-coord 2>&1 | grep -c '^failover '" || true)
echo "failovers: $failovers, kills: $kills"
mkdir -p bench/cloud/results
rsh control 'cat results/chaos.csv' >"bench/cloud/chaos-$mode.csv" || true
((status)) && exit 1
echo "chaos($mode): PASS"
