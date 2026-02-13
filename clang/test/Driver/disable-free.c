// Test -disable-free / -no-disable-free behavior in the clang driver.
//
// The driver adds -disable-free to cc1 commands by default (both in-process
// and out-of-process). The -Xclang -no-disable-free override is respected
// in all modes and suppresses the automatic -disable-free addition.

// REQUIRES: x86-registered-target

//--- Default (out-of-process): -disable-free should be present.
// RUN: %clang --target=x86_64-linux -### -S %s -o %t.s 2>&1 \
// RUN:     | FileCheck %s --check-prefix=DEFAULT
// DEFAULT: "-disable-free"

//--- Explicit -fno-integrated-cc1: -disable-free should be present.
// RUN: %clang --target=x86_64-linux -fno-integrated-cc1 -### -S %s -o %t.s 2>&1 \
// RUN:     | FileCheck %s --check-prefix=SPAWN
// SPAWN: "-disable-free"

//--- Explicit -fintegrated-cc1: -disable-free is still present (cc1 always
//--- gets -disable-free by default, regardless of in-process mode).
// RUN: %clang --target=x86_64-linux -fintegrated-cc1 -fintegrated-as -### -S %s -o %t.s 2>&1 \
// RUN:     | FileCheck %s --check-prefix=INTEGRATED
// INTEGRATED: "-disable-free"

//--- Explicit override with -Xclang -no-disable-free in spawned mode.
//--- When the user explicitly passes -no-disable-free, the driver should NOT
//--- add its own -disable-free.
// RUN: %clang --target=x86_64-linux -fno-integrated-cc1 -Xclang -no-disable-free \
// RUN:     -### -S %s -o %t.s 2>&1 \
// RUN:     | FileCheck %s --check-prefix=OVERRIDE-SPAWN
// OVERRIDE-SPAWN: "-no-disable-free"

//--- Explicit override with -Xclang -no-disable-free in integrated mode.
// RUN: %clang --target=x86_64-linux -fintegrated-cc1 -fintegrated-as \
// RUN:     -Xclang -no-disable-free -### -S %s -o %t.s 2>&1 \
// RUN:     | FileCheck %s --check-prefix=OVERRIDE-INTEGRATED
// OVERRIDE-INTEGRATED: "-no-disable-free"

int x;
