output "vm_name" {
  description = "Name of the build VM."
  value       = google_compute_instance.llvm_builder.name
}

output "vm_zone" {
  description = "Zone of the build VM."
  value       = google_compute_instance.llvm_builder.zone
}

output "bucket_url" {
  description = "GCS bucket URL where build artifacts are uploaded."
  value       = "gs://${google_storage_bucket.artifacts.name}/"
}

output "check_status" {
  description = "Command to check the build status via guest attributes."
  value       = "gcloud compute instances get-guest-attributes ${google_compute_instance.llvm_builder.name} --zone ${var.zone} --query-path=llvm-build/"
}

output "serial_log" {
  description = "Command to view the VM serial port output (build log)."
  value       = "gcloud compute instances get-serial-port-output ${google_compute_instance.llvm_builder.name} --zone ${var.zone}"
}

output "list_artifacts" {
  description = "Command to list uploaded build artifacts."
  value       = "gcloud storage ls gs://${google_storage_bucket.artifacts.name}/"
}
