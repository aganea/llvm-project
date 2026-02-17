# =============================================================================
# GCP Terraform configuration for building LLVM release packages on Windows.
#
# Spins up a Windows Server 2025 VM, clones the LLVM repo, runs the build
# script unattended, uploads artifacts to a GCS bucket, and stops the VM.
#
# See README.md for usage instructions.
# =============================================================================

terraform {
  required_version = ">= 1.3"
  required_providers {
    google = {
      source  = "hashicorp/google"
      version = ">= 5.0"
    }
    random = {
      source  = "hashicorp/random"
      version = ">= 3.0"
    }
  }
}

provider "google" {
  project = var.project_id
  region  = regex("^(.*)-[a-z]$", var.zone)[0]
}

# -----------------------------------------------------------------------------
# Random suffix for auto-generated bucket name
# -----------------------------------------------------------------------------

resource "random_id" "bucket_suffix" {
  byte_length = 4
}

locals {
  bucket_name = var.bucket_name != "" ? var.bucket_name : "llvm-build-${random_id.bucket_suffix.hex}"
  vm_name     = "llvm-builder"

  # Convert architectures list to PowerShell switch flags: @("-x64", "-arm64")
  arch_flags = join(" ", [for a in var.architectures : "-${a}"])
}

# -----------------------------------------------------------------------------
# GCS bucket for build artifacts
# -----------------------------------------------------------------------------

resource "google_storage_bucket" "artifacts" {
  name                        = local.bucket_name
  location                    = regex("^(.*)-[a-z]$", var.zone)[0]
  force_destroy               = true
  uniform_bucket_level_access = true

  lifecycle_rule {
    condition {
      age = var.artifact_retention_days
    }
    action {
      type = "Delete"
    }
  }
}

# -----------------------------------------------------------------------------
# Service account for the build VM
# -----------------------------------------------------------------------------

resource "google_service_account" "builder" {
  account_id   = "llvm-builder"
  display_name = "LLVM Release Builder"
}

resource "google_storage_bucket_iam_member" "builder_write" {
  bucket = google_storage_bucket.artifacts.name
  role   = "roles/storage.objectAdmin"
  member = "serviceAccount:${google_service_account.builder.email}"
}

resource "google_project_iam_member" "builder_compute" {
  project = var.project_id
  role    = "roles/compute.instanceAdmin.v1"
  member  = "serviceAccount:${google_service_account.builder.email}"
}

resource "google_project_iam_member" "builder_sa_user" {
  project = var.project_id
  role    = "roles/iam.serviceAccountUser"
  member  = "serviceAccount:${google_service_account.builder.email}"
}

# -----------------------------------------------------------------------------
# Windows Server 2025 build VM
# -----------------------------------------------------------------------------

resource "google_compute_instance" "llvm_builder" {
  name         = local.vm_name
  machine_type = var.machine_type
  zone         = var.zone

  boot_disk {
    initialize_params {
      # "windows-2025" is the Desktop Experience / Datacenter family (image
      # names look like windows-server-2025-dc-vNNNNNNNN); the Server Core
      # variant is the separate "windows-2025-core" family.
      image = "windows-cloud/windows-2025"
      size  = var.disk_size_gb
      type  = "pd-ssd"
    }
  }

  network_interface {
    network = "default"
    access_config {} # Ephemeral public IP (needed for downloads)
  }

  service_account {
    email  = google_service_account.builder.email
    scopes = ["cloud-platform"]
  }

  metadata = {
    # The startup script is rendered from a template file.
    windows-startup-script-ps1 = templatefile("${path.module}/startup.ps1.tftpl", {
      git_repo      = var.git_repo
      git_ref       = var.git_ref
      arch_flags    = local.arch_flags
      llvm_version  = var.llvm_version
      bucket_name   = google_storage_bucket.artifacts.name
      vm_name       = local.vm_name
      zone          = var.zone
    })

    enable-guest-attributes    = "TRUE"
    serial-port-logging-enable = "TRUE"
  }

  # Allow the VM time to build before Terraform considers it failed.
  timeouts {
    create = "4h"
  }

  # Prevent Terraform from recreating the VM on startup script changes.
  lifecycle {
    ignore_changes = [metadata["windows-startup-script-ps1"]]
  }
}
