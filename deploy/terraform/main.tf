terraform {
  required_version = ">= 1.5"
  required_providers {
    azurerm = {
      source  = "hashicorp/azurerm"
      version = "~> 4.0"
    }
  }
}

provider "azurerm" {
  features {}
  # Set ARM_SUBSCRIPTION_ID or run `az login` and export it; azurerm v4 requires it.
}

locals {
  prefix = "kretrieve"
  # Static private IPs so scripts and the coordinator's --shards list are known up front.
  # "control" runs the coordinator and the load generator; shard VMs run the shard processes.
  subnet_cidr = "10.0.1.0/24"
  control_ip  = cidrhost(local.subnet_cidr, 10)
  shard_ips   = [for i in range(var.shard_vm_count) : cidrhost(local.subnet_cidr, 20 + i)]

  vms = merge(
    { control = local.control_ip },
    { for i, ip in local.shard_ips : "shard-vm${i}" => ip },
  )

  # Logical shard i: primary on shard VM (i mod V) at port 9000+i; replica on the next VM
  # ((i+1) mod V) at port 9100+i, so losing any one VM leaves a live copy of every shard.
  # With a single shard VM there is nowhere to put a replica, so none are created.
  shards   = [for i in range(var.shard_count) : "${local.shard_ips[i % var.shard_vm_count]}:${9000 + i}"]
  replicas = var.shard_vm_count < 2 ? [] : [for i in range(var.shard_count) : "${local.shard_ips[(i + 1) % var.shard_vm_count]}:${9100 + i}"]
}

resource "azurerm_resource_group" "rg" {
  name     = "${local.prefix}-rg"
  location = var.location
  tags     = { project = "kretrieve", purpose = "benchmark" }
}

resource "azurerm_virtual_network" "vnet" {
  name                = "${local.prefix}-vnet"
  location            = azurerm_resource_group.rg.location
  resource_group_name = azurerm_resource_group.rg.name
  address_space       = ["10.0.0.0/16"]
}

resource "azurerm_subnet" "subnet" {
  name                 = "${local.prefix}-subnet"
  resource_group_name  = azurerm_resource_group.rg.name
  virtual_network_name = azurerm_virtual_network.vnet.name
  address_prefixes     = [local.subnet_cidr]
}

# Azure's default NSG rules already allow VNet-internal traffic and deny other inbound, so the shard
# and coordinator ports (no auth/TLS) are only reachable inside the subnet. Add SSH from one IP only.
resource "azurerm_network_security_group" "nsg" {
  name                = "${local.prefix}-nsg"
  location            = azurerm_resource_group.rg.location
  resource_group_name = azurerm_resource_group.rg.name

  security_rule {
    name                       = "ssh-from-me"
    priority                   = 100
    direction                  = "Inbound"
    access                     = "Allow"
    protocol                   = "Tcp"
    source_port_range          = "*"
    destination_port_range     = "22"
    source_address_prefix      = var.allowed_ssh_cidr
    destination_address_prefix = "*"
  }
}

resource "azurerm_subnet_network_security_group_association" "nsg" {
  subnet_id                 = azurerm_subnet.subnet.id
  network_security_group_id = azurerm_network_security_group.nsg.id
}

resource "azurerm_public_ip" "pip" {
  for_each            = local.vms
  name                = "${local.prefix}-${each.key}-pip"
  location            = azurerm_resource_group.rg.location
  resource_group_name = azurerm_resource_group.rg.name
  allocation_method   = "Static"
  sku                 = "Standard"
  zones               = [var.zone]
}

resource "azurerm_network_interface" "nic" {
  for_each            = local.vms
  name                = "${local.prefix}-${each.key}-nic"
  location            = azurerm_resource_group.rg.location
  resource_group_name = azurerm_resource_group.rg.name

  ip_configuration {
    name                          = "primary"
    subnet_id                     = azurerm_subnet.subnet.id
    private_ip_address_allocation = "Static"
    private_ip_address            = each.value
    public_ip_address_id          = azurerm_public_ip.pip[each.key].id
  }
}

resource "azurerm_linux_virtual_machine" "vm" {
  for_each                        = local.vms
  name                            = "${local.prefix}-${each.key}"
  location                        = azurerm_resource_group.rg.location
  resource_group_name             = azurerm_resource_group.rg.name
  size                            = each.key == "control" ? var.control_vm_size : var.vm_size
  zone                            = var.zone
  admin_username                  = "azureuser"
  disable_password_authentication = true
  network_interface_ids           = [azurerm_network_interface.nic[each.key].id]
  custom_data                     = base64encode(templatefile("${path.module}/cloud-init.yaml", { image = var.image }))
  tags                            = { role = each.key }

  admin_ssh_key {
    username   = "azureuser"
    public_key = file(pathexpand(var.ssh_public_key_path))
  }

  os_disk {
    caching              = "ReadWrite"
    storage_account_type = "StandardSSD_LRS"
  }

  source_image_reference {
    publisher = "Canonical"
    offer     = "ubuntu-24_04-lts"
    sku       = "server"
    version   = "latest"
  }
}

# Safety net against a forgotten, still-billing cluster. `terraform destroy` is the real cleanup.
resource "azurerm_dev_test_global_vm_shutdown_schedule" "shutdown" {
  for_each           = local.vms
  virtual_machine_id = azurerm_linux_virtual_machine.vm[each.key].id
  location           = azurerm_resource_group.rg.location
  enabled            = true

  daily_recurrence_time = var.shutdown_time_utc
  timezone              = "UTC"

  notification_settings {
    enabled = false
  }
}

resource "azurerm_consumption_budget_resource_group" "budget" {
  count             = var.budget_alert_email == "" ? 0 : 1
  name              = "${local.prefix}-budget"
  resource_group_id = azurerm_resource_group.rg.id
  amount            = var.budget_usd
  time_grain        = "Monthly"

  time_period {
    start_date = formatdate("YYYY-MM-01'T'00:00:00'Z'", timestamp())
  }

  notification {
    enabled        = true
    threshold      = 50
    operator       = "GreaterThan"
    threshold_type = "Actual"
    contact_emails = [var.budget_alert_email]
  }

  notification {
    enabled        = true
    threshold      = 90
    operator       = "GreaterThan"
    threshold_type = "Actual"
    contact_emails = [var.budget_alert_email]
  }

  lifecycle {
    ignore_changes = [time_period]
  }
}
