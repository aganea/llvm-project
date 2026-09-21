// Verify that static-arena selection remains tied to the materialized storage
// name when later CodeGen paths query the same declaration.
//
// RUN: split-file %s %t
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/test.c -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   %t/local.c -o - \
// RUN:   | FileCheck %s --implicit-check-not='@local_pointer.target ='

//--- arena.list
[arena]
global:arena_target
global:function.p
global:local_pointer.target

//--- test.c
int arena_target = 1;

void function(void) {
  static int *p = // expected-error {{static initializer template for p refers to static-arena variable arena_target}}
      &arena_target;
  (void)p;
}

//--- local.c

int *local_pointer(void) {
  static int target;
  int *const pointer = &target;
  return pointer;
}

// A later constant-valued automatic initializer must preserve the selection
// made with the materialized C local-static name, not rematch "target".
// CHECK-LABEL: define {{.*}}ptr @local_pointer
// CHECK: call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1.local_pointer.target)
