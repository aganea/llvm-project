// Make sure LIBCMT doesn't accidentally get added to the list of DEFAULTLIB
// directives.
//
// Inspect the compiled object's own .drectve section directly instead of
// parsing link.exe's "Creating library" banner (not printed by lld-link) or
// any driver/linker text output, neither of which is a stable interface to
// test against.

// REQUIRES: asan-dynamic-runtime
// RUN: %clang_cl_asan -LD -c %s -o %t.obj
// RUN: llvm-readobj --coff-directives %t.obj | FileCheck %s
// CHECK: Directive(s):
// CHECK-NOT: LIBCMT

void foo(int *p) { *p = 42; }

__declspec(dllexport) void bar() {
  int x;
  foo(&x);
}
