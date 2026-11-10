variable "location" {
  description = "Azure region. Azure for Students only allows japaneast, eastasia, japanwest, indonesiacentral and malaysiawest. Every VM goes in this region and zone."
  type        = string
  default     = "japaneast"
}

variable "zone" {
  description = "Availability zone for every VM and public IP (keeps network latency uniform)."
  type        = string
  default     = "1"
}

variable "vm_size" {
  description = "Shard VM size. Non-burstable (B-series throttles when CPU credits run out and would skew benchmarks). Azure for Students has 0 quota for the v5 families and 4 vCPUs per older family, so two 2-vCPU shard VMs fit in DSv4."
  type        = string
  default     = "Standard_D2s_v4"
}

variable "control_vm_size" {
  description = "Control VM (coordinator + loadgen). A different family from the shard VMs, so the per-family 4-vCPU quota is not exceeded; same generation keeps CPUs comparable."
  type        = string
  default     = "Standard_D2ds_v4"
}

variable "shard_vm_count" {
  description = "Shard VMs (2 vCPU each). Plus one control VM = 2 vCPU x (count + 1); Azure for Students caps a region at 6 vCPUs, so max 2. Use 1 for the single-machine side of the scaling test (no replicas then)."
  type        = number
  default     = 2
  validation {
    condition     = var.shard_vm_count >= 1 && var.shard_vm_count <= 8
    error_message = "shard_vm_count must be between 1 and 8."
  }
}

variable "shard_count" {
  description = "Logical shards. Several can share a VM; keep it equal to the README's 3 to compare with the single-machine numbers."
  type        = number
  default     = 3
  validation {
    condition     = var.shard_count >= 1 && var.shard_count <= 16
    error_message = "shard_count must be between 1 and 16."
  }
}

variable "allowed_ssh_cidr" {
  description = "Your public IP as a CIDR, e.g. 203.0.113.7/32. SSH is only reachable from here."
  type        = string
  validation {
    condition     = var.allowed_ssh_cidr != "0.0.0.0/0" && !strcontains(var.allowed_ssh_cidr, ":") && can(cidrhost(var.allowed_ssh_cidr, 0))
    error_message = "Give a valid IPv4 CIDR (the VNet is IPv4-only; try `curl -4 -s https://ifconfig.me`) and do not open SSH to the whole internet."
  }
}

variable "ssh_public_key_path" {
  description = "Path to the SSH public key installed for the admin user."
  type        = string
  default     = "~/.ssh/id_ed25519.pub"
}

variable "image" {
  description = "Container image pulled by every VM (pushed by CI on v* tags)."
  type        = string
  default     = "ghcr.io/nglong14/kretrieve:latest"
}

variable "shutdown_time_utc" {
  description = "Daily auto-shutdown time (HHMM, UTC): a safety net against forgotten VMs."
  type        = string
  default     = "2300"
}

variable "budget_usd" {
  description = "Monthly budget for the resource group; alerts at 50% and 90% of actual spend."
  type        = number
  default     = 10
}

variable "budget_alert_email" {
  description = "Email for budget alerts. Leave empty to skip creating the budget."
  type        = string
  default     = ""
}
