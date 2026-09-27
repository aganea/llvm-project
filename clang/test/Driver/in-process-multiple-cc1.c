// REQUIRES: llvm-driver, webassembly-registered-target

// A Clang invocation owned by ToolSession can execute multiple cc1 jobs
// in-process. Each job must free its CompilerInstance before returning.
// RUN: split-file %s %t

// An implicit dry run retains the standalone driver's output contract even
// though the session will restore these jobs to in-process execution when it
// actually runs them. In particular, the commands remain adjacent.
// RUN: cd %t && %clang --target=wasm32-unknown-unknown \
// RUN:   -c -### first.c second.c 2>&1 \
// RUN:   | FileCheck %s --check-prefix=IMPLICIT \
// RUN:       --implicit-check-not='(in-process)'
// IMPLICIT: "-cc1"
// IMPLICIT-SAME: "first.c"
// IMPLICIT-NEXT: {{.*}}"-cc1"
// IMPLICIT-SAME: "second.c"

// RUN: cd %t && %clang --target=wasm32-unknown-unknown \
// RUN:   -fintegrated-cc1 -c -### first.c second.c 2>&1 \
// RUN:   | FileCheck %s --check-prefix=COMMANDS \
// RUN:       --implicit-check-not='"-disable-free"'
// COMMANDS-COUNT-2: (in-process)

// The same cleanup rule applies to a single cc1 job because the session stays
// alive after the top-level Clang invocation returns.
// RUN: cd %t && %clang --target=wasm32-unknown-unknown \
// RUN:   -fintegrated-cc1 -c -### first.c 2>&1 \
// RUN:   | FileCheck %s --check-prefix=SINGLE \
// RUN:       --implicit-check-not='"-disable-free"'
// SINGLE: (in-process)

// A compile-and-link invocation also keeps cc1 in-process even though it has a
// second job for the linker.
// RUN: cd %t && %clang --target=wasm32-unknown-unknown -nostdlib \
// RUN:   -fintegrated-cc1 -### first.c 2>&1 \
// RUN:   | FileCheck %s --check-prefix=LINK \
// RUN:       --implicit-check-not='"-disable-free"'
// LINK: (in-process)

// An explicit request for a separate cc1 process remains authoritative.
// RUN: cd %t && %clang --target=wasm32-unknown-unknown \
// RUN:   -fno-integrated-cc1 -c -### first.c second.c 2>&1 \
// RUN:   | FileCheck %s --check-prefix=SPAWN \
// RUN:       --implicit-check-not='(in-process)'
// SPAWN-COUNT-2: "-disable-free"

// Exercise the jobs and verify that both objects were emitted successfully.
// RUN: cd %t && %clang --target=wasm32-unknown-unknown \
// RUN:   -fintegrated-cc1 -c first.c second.c
// RUN: llvm-readobj --file-headers %t/first.o %t/second.o \
// RUN:   | FileCheck %s --check-prefix=OBJECTS
// OBJECTS-COUNT-2: Format: WASM

// Invalid cc1 arguments must release the fatal-error handler before the
// next in-process job installs its own handler.
// RUN: cd %t && not %clang --target=wasm32-unknown-unknown \
// RUN:   -fintegrated-cc1 -fsyntax-only -Xclang -invalid-cc1-option \
// RUN:   first.c second.c 2>&1 | FileCheck %s --check-prefix=INVALID-CC1
// INVALID-CC1-COUNT-2: error: unknown argument: '-invalid-cc1-option'

//--- first.c
int first(void) { return 1; }

//--- second.c
int second(void) { return 2; }
