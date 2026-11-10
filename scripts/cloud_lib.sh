#!/usr/bin/env bash
# Shared helpers for the cloud scripts; source it from the repo root after `terraform apply`.
# Env: IMAGE (local image tag loaded onto the VMs, default kretrieve), SSH_KEY (private key path),
#      TF_DIR (deploy/terraform).

tf_dir=${TF_DIR:-deploy/terraform}
image=${IMAGE:-kretrieve}
rhome=/home/azureuser
ssh_opts=(-o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 -o BatchMode=yes
  -o ServerAliveInterval=15)
[[ -n ${SSH_KEY:-} ]] && ssh_opts+=(-i "$SSH_KEY")

die() { echo "error: $*" >&2; exit 1; }
tfo() { terraform -chdir="$tf_dir" output "$@"; }

# init_cluster: fill pub[vm], priv[vm], shard_vms, shard_addrs[i], replica_addrs[i] from Terraform.
init_cluster() {
  declare -gA pub priv
  local name p q
  while read -r name p q; do
    pub[$name]=$p
    priv[$name]=$q
  done < <(python3 -c '
import json, sys
public, private = map(json.loads, sys.argv[1:])
for name in public:
    print(name, public[name], private[name])
' "$(tfo -json public_ips)" "$(tfo -json private_ips)")
  [[ -n ${pub[control]:-} ]] || die "no control VM in Terraform outputs; run terraform apply first"
  IFS=, read -ra shard_addrs <<<"$(tfo -raw shards_arg)"
  IFS=, read -ra replica_addrs <<<"$(tfo -raw replicas_arg)"
  shard_vms=()
  for name in "${!pub[@]}"; do [[ $name == shard-vm* ]] && shard_vms+=("$name"); done
  all_vms=(control "${shard_vms[@]}")
}

vm_of() { # <private ip> -> VM name
  local k
  for k in "${!priv[@]}"; do [[ ${priv[$k]} == "$1" ]] && { echo "$k"; return; }; done
  die "no VM has private IP $1"
}

rsh() { # <vm> <command string>
  local vm=$1
  shift
  ssh "${ssh_opts[@]}" "azureuser@${pub[$vm]}" "$@"
}

# Remote docker run for the load generator on the control VM; extra args are loadgen flags.
run_loadgen() { # <out csv name> <qps> <duration s> <budget us>
  rsh control "docker run --rm --network host -v $rhome/data:/data:ro -v $rhome/results:/out \
    $image loadgen --host 127.0.0.1 --port 8000 --qps $2 --duration $3 \
    --queries /data/queries.txt --budget-us $4 --connections 48 --out /out/$1"
}
