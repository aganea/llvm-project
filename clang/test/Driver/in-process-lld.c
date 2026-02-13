// Test in-process lld-link invocation from the clang driver.
// When building in llvm-driver mode, lld-link should be invoked in-process
// and receive -no-disable-free to ensure proper resource cleanup.

// REQUIRES: x86-registered-target, lld, llvm-driver

//--- In-process lld-link: should show (in-process) and -no-disable-free.
// RUN: %clang --target=x86_64-pc-windows-msvc -fuse-ld=lld -### %s 2>&1 \
// RUN:     | FileCheck %s --check-prefix=INPROCESS
// INPROCESS: (in-process)
// INPROCESS: (in-process)
// INPROCESS-NEXT: "{{.*}}lld-link
// INPROCESS-SAME: "-no-disable-free"

//--- When -fno-integrated-cc1 is used, cc1 should not be in-process but
//--- lld-link may still be (it's controlled by the driver's InProcess flag,
//--- not -fintegrated-cc1).
// RUN: %clang --target=x86_64-pc-windows-msvc -fuse-ld=lld \
// RUN:     -fno-integrated-cc1 -### %s 2>&1 \
// RUN:     | FileCheck %s --check-prefix=NOCC1
// NOCC1-NOT: (in-process){{.*}}"-cc1"
// NOCC1: "-cc1"

//--- When -fno-integrated-tools is used, no tools should be in-process
//--- (neither cc1 nor lld-link).
// RUN: %clang --target=x86_64-pc-windows-msvc -fuse-ld=lld \
// RUN:     -fno-integrated-tools -### %s 2>&1 \
// RUN:     | FileCheck %s --check-prefix=NOTOOLS
// NOTOOLS-NOT: (in-process)

// Note: The out-of-process lld-link path uses UTF-16 response files
// (ResponseFileSupport::AtFileUTF16()) for compatibility with MSVC's link.exe.
// This encoding difference is not observable via -### dry-run output, but is
// validated by the response file writing code path at execution time.

int main() { return 0; }
