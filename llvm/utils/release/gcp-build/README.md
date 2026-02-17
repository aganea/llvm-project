# LLVM Release Build on GCP

Terraform configuration that spins up a Windows Server 2025 VM on Google Cloud,
builds LLVM release packages unattended, uploads the artifacts to a GCS bucket,
and stops the VM when done.

## Prerequisites

- [Terraform](https://developer.hashicorp.com/terraform/install) >= 1.3
- [Google Cloud SDK](https://cloud.google.com/sdk/docs/install) (`gcloud`)
- A GCP project with billing enabled
- Authenticated via `gcloud auth application-default login`
- The following GCP APIs enabled on the project:
  - Compute Engine API (`compute.googleapis.com`)
  - Cloud Storage API (`storage.googleapis.com`)
  - IAM API (`iam.googleapis.com`)

## Quick Start

```bash
cd llvm/utils/release/gcp-build

terraform init

# Build x64 from the main branch
terraform apply -var="project_id=my-gcp-project"
```

## Usage

### Build x64 from main (default)

```bash
terraform apply -var="project_id=my-gcp-project"
```

### Build a specific release version

```bash
terraform apply \
  -var="project_id=my-gcp-project" \
  -var="llvm_version=19.1.0" \
  -var="git_ref=llvmorg-19.1.0"
```

### Build multiple architectures

```bash
terraform apply \
  -var="project_id=my-gcp-project" \
  -var='architectures=["x64","x86","arm64"]'
```

### Build a specific version for all architectures

```bash
terraform apply \
  -var="project_id=my-gcp-project" \
  -var="llvm_version=19.1.0" \
  -var="git_ref=llvmorg-19.1.0" \
  -var='architectures=["x64","x86","arm64"]'
```

### Build from a custom repo/branch

```bash
terraform apply \
  -var="project_id=my-gcp-project" \
  -var="git_repo=https://github.com/user/llvm-project.git" \
  -var="git_ref=my-feature-branch"
```

## Monitoring the Build

After `terraform apply` completes, the VM boots and starts building. The build
runs asynchronously -- Terraform returns once the VM is created, not when the
build finishes.

### Check build status

The VM writes its current status to a guest attribute:

```bash
gcloud compute instances get-guest-attributes llvm-builder \
  --zone us-central1-a --query-path=llvm-build/
```

Status values: `starting`, `cloning`, `building`, `uploading`, `success`,
`failed:<exit_code>`.

### View live build output

Serial port output captures the VM console:

```bash
gcloud compute instances get-serial-port-output llvm-builder \
  --zone us-central1-a
```

### View the full build log

After the build completes (success or failure), the full log is uploaded to
the GCS bucket:

```bash
gcloud storage cat gs://$(terraform output -raw bucket_url)build_log.txt
```

### List uploaded artifacts

```bash
gcloud storage ls gs://$(terraform output -raw bucket_url)
```

## Downloading Artifacts

```bash
# Download all artifacts
gcloud storage cp "gs://$(terraform output -raw bucket_url)*" .

# Download just the x64 installer
gcloud storage cp "gs://$(terraform output -raw bucket_url)LLVM-*-win64.exe" .
```

## Cleaning Up

The VM automatically stops after the build finishes to save costs. To remove
all resources (VM, bucket, service account):

```bash
terraform destroy -var="project_id=my-gcp-project"
```

## Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `project_id` | (required) | GCP project ID |
| `zone` | `us-central1-a` | GCP zone for the VM |
| `machine_type` | `n4-standard-64` | VM machine type (64 vCPUs, 256 GB RAM) |
| `disk_size_gb` | `512` | Boot disk size in GB |
| `llvm_version` | `""` | LLVM version (empty = auto-detect) |
| `architectures` | `["x64"]` | Architectures to build: `x64`, `x86`, `arm64` |
| `git_repo` | `https://github.com/llvm/llvm-project.git` | Git repo URL |
| `git_ref` | `main` | Git branch, tag, or commit |
| `bucket_name` | `""` | GCS bucket name (empty = auto-generate) |
| `artifact_retention_days` | `30` | Days before artifacts are auto-deleted |

## Outputs

| Output | Description |
|--------|-------------|
| `vm_name` | Name of the build VM |
| `vm_zone` | Zone of the build VM |
| `bucket_url` | GCS bucket URL for artifacts |
| `check_status` | `gcloud` command to check build status |
| `serial_log` | `gcloud` command to view serial port output |
| `list_artifacts` | `gcloud storage` command to list uploaded artifacts |

## How It Works

1. `terraform apply` creates a GCS bucket, service account, and Windows Server
   2025 VM.
2. The VM boots and runs a startup script that:
   - Installs Git and clones the LLVM repository (shallow clone of the
     specified ref).
   - Runs `build_llvm_release.ps1 -InstallPrerequisites` which installs all
     build tools (Visual Studio, CMake, Ninja, Python, NSIS, etc.) and then
     builds LLVM with PGO optimization.
   - Uploads the resulting `.exe` installers and `.tar.xz` tarballs to the
     GCS bucket.
   - Writes build status to a guest attribute for easy monitoring.
   - Stops the VM to save costs.
3. The user downloads artifacts from the bucket and runs `terraform destroy`
   to clean up.

## Cost Estimate

An `n4-standard-64` VM in `us-central1` costs approximately $3.50/hour for
Windows Server. A full x64 PGO build typically takes 1-2 hours. Building all
three architectures sequentially takes 3-5 hours. The 512 GB SSD disk adds
about $0.10/hour. Total cost for a single x64 build: approximately $5-10.
