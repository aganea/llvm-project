// REQUIRES: llvm-driver
// REQUIRES: lld

// Only one TU, but we're linking with LLD, two jobs, integrated-cc1 is still enabled for both jobs.
// RUN: %clang -fintegrated-cc1 -fuse-ld=lld %s -### 2>&1 | FileCheck %s --check-prefix=YES2
// YES2:  (in-process)
// YES2:  (in-process)
