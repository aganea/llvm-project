variable "project_id" {
  description = "GCP project ID where the build VM and bucket will be created."
  type        = string
}

variable "zone" {
  description = "GCP zone for the build VM."
  type        = string
  default     = "us-central1-a"
}

variable "machine_type" {
  description = "GCP machine type for the build VM. N4 with 64 vCPUs recommended for fast builds."
  type        = string
  default     = "n4-standard-64"
}

variable "disk_size_gb" {
  description = "Boot disk size in GB. 512 GB accommodates all three architectures."
  type        = number
  default     = 512
}

variable "llvm_version" {
  description = "LLVM version string (e.g. '19.1.0'). Empty = auto-detect from source tree."
  type        = string
  default     = ""
}

variable "architectures" {
  description = "List of architectures to build. Valid values: x64, x86, arm64."
  type        = list(string)
  default     = ["x64"]

  validation {
    condition     = alltrue([for a in var.architectures : contains(["x64", "x86", "arm64"], a)])
    error_message = "Each architecture must be one of: x64, x86, arm64."
  }
}

variable "git_repo" {
  description = "Git repository URL to clone."
  type        = string
  default     = "https://github.com/llvm/llvm-project.git"
}

variable "git_ref" {
  description = "Git branch, tag, or commit to check out (e.g. 'main', 'llvmorg-19.1.0')."
  type        = string
  default     = "main"
}

variable "bucket_name" {
  description = "GCS bucket name for build artifacts. Empty = auto-generate a unique name."
  type        = string
  default     = ""
}

variable "artifact_retention_days" {
  description = "Number of days to retain artifacts in the GCS bucket before auto-deletion."
  type        = number
  default     = 30
}
