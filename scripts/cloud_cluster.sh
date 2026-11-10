#!/usr/bin/env bash
# Manage the KRetrieve cluster on the Terraform-created Azure VMs.
# Usage: scripts/cloud_cluster.sh images | data | up [shard flags...] | down | status
#   images  load the local docker image onto every VM (no registry needed)
#   data    copy the prepared shards to the shard VMs, queries to the control VM, and verify
#           that global_stats.bin is byte-identical everywhere
#   up      (re)start shards, optional replicas and the coordinator; extra args go to every
#           shard, e.g. up --latency-ms 20-100 --latency-prob 0.1
#   down    remove all kr-* containers
# Env: WITH_REPLICAS=1 (also start replicas and pass --replicas), HEDGE_MS=<n> (implies replicas
#      and adds --hedge-ms), RESTART=no|unless-stopped (docker restart policy, default no),
#      PREPARED (bench/data/prepared), QUERIES (bench/data/queries.txt), IMAGE, SSH_KEY.
# Containers use host networking, so Docker NAT does not distort latency.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/cloud_lib.sh
cmd=${1:?usage: scripts/cloud_cluster.sh images|data|up|down|status}
shift
init_cluster

wait_ready() { # cloud-init must have installed docker
  local vm
  for vm in "${all_vms[@]}"; do
    rsh "$vm" 'cloud-init status --wait >/dev/null 2>&1 || true; docker info >/dev/null' \
      || die "docker not ready on $vm"
  done
}

# wait_listening <vm> <container>: shards print "listening on" once the index is built.
wait_listening() {
  rsh "$1" "for _ in \$(seq 180); do docker logs $2 2>&1 | grep -q '^listening on' && exit 0; sleep 1; done; exit 1" \
    || die "$2 on $1 did not start; see: ssh azureuser@${pub[$1]} docker logs $2"
}

case $cmd in
images)
  docker image inspect "$image" >/dev/null 2>&1 || die "build the image first: docker build -t $image ."
  wait_ready
  for vm in "${all_vms[@]}"; do
    echo "loading $image onto $vm"
    docker save "$image" | gzip | rsh "$vm" 'gunzip | docker load' >/dev/null
  done
  ;;
data)
  prepared=${PREPARED:-bench/data/prepared}
  queries=${QUERIES:-bench/data/queries.txt}
  [[ -f $prepared/global_stats.bin && -f $queries ]] || die "missing $prepared or $queries"
  count=$(find "$prepared" -name 'shard-*.tsv' | wc -l)
  ((count == ${#shard_addrs[@]})) \
    || die "$prepared has $count partitions but Terraform expects ${#shard_addrs[@]} shards"
  want=$(sha256sum "$prepared/global_stats.bin" | cut -d' ' -f1)
  wait_ready
  for vm in "${shard_vms[@]}"; do
    echo "copying shards to $vm"
    rsh "$vm" 'rm -rf data && mkdir -p data/prepared'
    tar -C "$prepared" -cf - . | rsh "$vm" 'tar -C data/prepared -xf -'
    got=$(rsh "$vm" 'sha256sum data/prepared/global_stats.bin' | cut -d' ' -f1)
    [[ $got == "$want" ]] || die "global_stats.bin differs on $vm"
  done
  rsh control 'rm -rf data results && mkdir -p data results && chmod 777 results'
  rsh control 'cat > data/queries.txt' <"$queries"
  echo "data ok: global_stats.bin sha256 ${want:0:12}... on all ${#shard_vms[@]} shard VMs"
  ;;
down)
  for vm in "${all_vms[@]}"; do
    rsh "$vm" 'docker ps -aq --filter name=kr- | xargs -r docker rm -f >/dev/null'
  done
  ;;
status)
  for vm in "${all_vms[@]}"; do
    echo "== $vm"
    rsh "$vm" "docker ps -a --filter name=kr- --format '{{.Names}}  {{.Status}}'"
  done
  ;;
up)
  [[ -n ${HEDGE_MS:-} ]] && WITH_REPLICAS=1
  with_replicas=${WITH_REPLICAS:-0}
  restart=${RESTART:-no}
  if ((with_replicas)) && [[ -z ${replica_addrs[0]:-} ]]; then
    die "no replicas in this topology (need 2+ shard VMs)"
  fi
  flags=
  # printf with no arguments would emit an empty '' word, which the shard rejects.
  (($#)) && printf -v flags '%q ' "$@"
  "$0" down
  # start_shard <role p|r> <index> <host:port>
  start_shard() {
    local name=kr-$1$2 addr=$3 vm
    vm=$(vm_of "${addr%:*}")
    rsh "$vm" "docker run -d --name $name --restart $restart --network host \
      -v $rhome/data:/data:ro $image shard --stats /data/prepared/global_stats.bin \
      --partition /data/prepared/shard-$2.tsv --host 0.0.0.0 --port ${addr##*:} $flags" >/dev/null
    started+=("$vm $name")
  }
  started=()
  for i in "${!shard_addrs[@]}"; do
    start_shard p "$i" "${shard_addrs[$i]}"
    ((with_replicas)) && start_shard r "$i" "${replica_addrs[$i]}"
  done
  for entry in "${started[@]}"; do wait_listening ${entry}; done
  coord_flags="--shards $(tfo -raw shards_arg)"
  ((with_replicas)) && coord_flags+=" --replicas $(tfo -raw replicas_arg)"
  [[ -n ${HEDGE_MS:-} ]] && coord_flags+=" --hedge-ms $HEDGE_MS"
  # Same small worker pool and queue as scripts/bench.sh so overload shows up as Overloaded replies.
  rsh control "docker run -d --name kr-coord --restart $restart --network host $image \
    coordinator --host 0.0.0.0 --port 8000 --workers 8 --max-queue 16 $coord_flags" >/dev/null
  wait_listening control kr-coord
  echo "cluster up: ${#shard_addrs[@]} shards, replicas=$with_replicas hedge_ms=${HEDGE_MS:-off}"
  ;;
*) die "unknown command $cmd" ;;
esac
