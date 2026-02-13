// Test -fintegrated-tools / -fno-integrated-tools flag.
//
// -fintegrated-tools is the generalized version of -fintegrated-cc1 that
// controls in-process execution for all tools (cc1, cc1as, lld, etc.).
// It defaults to ON, making in-process compilation the default.
//
// -fintegrated-cc1 / -fno-integrated-cc1 can override cc1-specific behavior.

// If a toolchain uses an external assembler, the test would fail because using
// an external assembler would increase job counts. Most toolchains in tree
// use integrated assembler, but we still support external assembler.
// So -fintegrated-as is specified explicitly when applicable.

//--- Default: -fintegrated-tools is ON, so cc1 runs in-process.
// RUN: %clang -fintegrated-as -c -### %s 2>&1 | FileCheck %s --check-prefix=YES
// RUN: %clang -fintegrated-tools -fintegrated-as -c -### %s 2>&1 | FileCheck %s --check-prefix=YES

//--- -fno-integrated-tools disables in-process execution for all tools.
// RUN: %clang -fno-integrated-tools -c -### %s 2>&1 | FileCheck %s --check-prefix=NO

//--- Last flag wins.
// RUN: %clang -fintegrated-tools -fno-integrated-tools -c -### %s 2>&1 \
// RUN:     | FileCheck %s --check-prefix=NO
// RUN: %clang -fno-integrated-tools -fintegrated-tools -fintegrated-as -c -### %s 2>&1 \
// RUN:     | FileCheck %s --check-prefix=YES

//--- Works with clang-cl mode.
// RUN: %clang_cl -fintegrated-tools -fintegrated-as -c -### -- %s 2>&1 \
// RUN:     | FileCheck %s --check-prefix=YES
// RUN: %clang_cl -fno-integrated-tools -c -### -- %s 2>&1 \
// RUN:     | FileCheck %s --check-prefix=NO

//--- -fintegrated-cc1 can override cc1 behavior when -fintegrated-tools is on.
// With -fintegrated-tools (default) but -fno-integrated-cc1, cc1 should NOT
// be in-process. Other tools (like lld) would still be in-process.
// RUN: %clang -fintegrated-tools -fno-integrated-cc1 -c -### %s 2>&1 \
// RUN:     | FileCheck %s --check-prefix=NO

//--- -fintegrated-cc1 can re-enable cc1 even when -fno-integrated-tools is set.
// This is backward compatible: -fintegrated-cc1 explicitly enables cc1 in-process.
// RUN: %clang -fno-integrated-tools -fintegrated-cc1 -fintegrated-as -c -### %s 2>&1 \
// RUN:     | FileCheck %s --check-prefix=YES

//--- -fintegrated-tools works with cc1as (assembler).
// macOS triples have an extra -x assembler-with-cpp job so (in-process) is
// not triggered. Use a Linux triple to avoid that.
// RUN: echo > %t.s
// RUN: %clang --target=x86_64-linux -fintegrated-tools -fintegrated-as -c -### %t.s 2>&1 \
// RUN:     | FileCheck %s --check-prefix=YES
// RUN: %clang --target=x86_64-linux -fno-integrated-tools -c -### %t.s 2>&1 \
// RUN:     | FileCheck %s --check-prefix=NO

//--- Multiple TUs: all cc1 jobs should be in-process with -fintegrated-tools.
// RUN: echo 'int f() { return 1; }' > %t1.c
// RUN: echo 'int g() { return 2; }' > %t2.c
// RUN: echo 'int h() { return 3; }' > %t3.c
// RUN: %clang -fintegrated-tools -c %t1.c %t2.c %t3.c -### 2>&1 \
// RUN:     | FileCheck %s --check-prefix=YES
// RUN: %clang -fno-integrated-tools -c %t1.c %t2.c %t3.c -### 2>&1 \
// RUN:     | FileCheck %s --check-prefix=NO

// YES: (in-process)
// NO-NOT: (in-process)
