output "public_ips" {
  description = "Public IP per VM, for SSH from allowed_ssh_cidr only."
  value       = { for k, p in azurerm_public_ip.pip : k => p.ip_address }
}

output "private_ips" {
  value = local.vms
}

output "coordinator_addr" {
  description = "Coordinator address inside the VNet (loadgen runs on the same control VM)."
  value       = "${local.control_ip}:8000"
}

output "shards_arg" {
  description = "Value for the coordinator's --shards."
  value       = join(",", local.shards)
}

output "replicas_arg" {
  description = "Value for the coordinator's --replicas (replica of shard i is on shard VM (i+1) mod V); empty with one shard VM."
  value       = join(",", local.replicas)
}

output "ssh" {
  value = { for k, p in azurerm_public_ip.pip : k => "ssh azureuser@${p.ip_address}" }
}
