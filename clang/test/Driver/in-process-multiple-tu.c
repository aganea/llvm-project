// Test that multiple translation units are compiled in-process by default,
// and that -fno-integrated-cc1 disables in-process execution for all jobs.

// REQUIRES: x86-registered-target

// Create multiple source files.
// RUN: echo 'int f() { return 1; }' > %t1.c
// RUN: echo 'int g() { return 2; }' > %t2.c
// RUN: echo 'int h() { return 3; }' > %t3.c

//--- Default: all cc1 jobs should be in-process.
// RUN: %clang --target=x86_64-linux -fintegrated-cc1 -fintegrated-as -c \
// RUN:     %t1.c %t2.c %t3.c -### 2>&1 \
// RUN:     | FileCheck %s --check-prefix=INPROCESS
// INPROCESS: (in-process)
// INPROCESS: "-cc1"
// INPROCESS: (in-process)
// INPROCESS: "-cc1"
// INPROCESS: (in-process)
// INPROCESS: "-cc1"

//--- With -fno-integrated-cc1: no cc1 jobs should be in-process.
// RUN: %clang --target=x86_64-linux -fno-integrated-cc1 -c \
// RUN:     %t1.c %t2.c %t3.c -### 2>&1 \
// RUN:     | FileCheck %s --check-prefix=NOINPROCESS
// NOINPROCESS-NOT: (in-process)
// NOINPROCESS: "-cc1"
// NOINPROCESS-NOT: (in-process)
// NOINPROCESS: "-cc1"
// NOINPROCESS-NOT: (in-process)
// NOINPROCESS: "-cc1"

//--- Single TU with linking: cc1 should be in-process, linker is separate.
// RUN: %clang --target=x86_64-linux -fintegrated-cc1 -fintegrated-as \
// RUN:     %t1.c -### 2>&1 \
// RUN:     | FileCheck %s --check-prefix=LINK
// LINK: (in-process)
// LINK: "-cc1"
