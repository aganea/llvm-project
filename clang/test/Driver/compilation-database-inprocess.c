// Test that /compilation-database mode produces in-process jobs with
// -working-directory injection when running in llvm-driver mode.

// REQUIRES: x86-registered-target, llvm-driver

//--- In llvm-driver mode, clang entries should be marked (in-process) and
//--- receive -working-directory and -no-disable-free.
// RUN: echo '[{"directory": "%/T", "file": "foo.c", "output": "foo.obj", "arguments": ["clang-cl", "/c", "foo.c", "/Fofoo.obj"]}]' > %t.cdb
// RUN: %clang_cl /compilation-database:%t.cdb -### 2>&1 | FileCheck %s --check-prefix=INPROCESS
// INPROCESS: (in-process)
// INPROCESS: "-no-disable-free"
// INPROCESS-SAME: "-working-directory"

//--- External tools like ml64 should NOT be run in-process.
// RUN: echo '[{"directory": "%/T", "file": "foo.asm", "output": "foo.obj", "arguments": ["ml64", "/c", "foo.asm", "/Fofoo.obj"]}]' > %t.ext.cdb
// RUN: %clang_cl /compilation-database:%t.ext.cdb -### 2>&1 | FileCheck %s --check-prefix=EXTERNAL
// EXTERNAL-NOT: (in-process)
