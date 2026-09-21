// ASan instruments resolver-derived accesses but must not create global
// metadata or redzones for the storage placeholder that is erased by Clang.
//
// RUN: split-file %s %t
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O0 \
// RUN:   -emit-llvm -fsanitize=address -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/test.cpp \
// RUN:   | FileCheck %s --check-prefix=ASAN \
// RUN:     --implicit-check-not='@arena_asan =' \
// RUN:     --implicit-check-not='@___asan_gen_global{{.*}}c"arena_asan\00"'

//--- arena.list
[arena]
global:*arena_asan*

//--- test.cpp
int arena_asan;
int process_global;

int read_write(int Value) {
  arena_asan = Value;
  process_global = Value;
  return arena_asan + process_global;
}

// The ordinary global is transformed and described by ASan, while no ASan
// descriptor names the relocated variable.
// ASAN-DAG: @process_global = global { i32, [28 x i8] }
// ASAN-DAG: @___asan_gen_global{{.*}} = private unnamed_addr constant {{.*}}c"process_global\00"

// Resolver-derived stores and loads retain the normal inline shadow checks and
// report calls. The record itself is not a replacement object access.
// ASAN-LABEL: define {{.*}}i32 @_Z10read_writei
// ASAN: call ptr @__llvm_arena_addr_v1
// ASAN: call void @__asan_report_store4
// ASAN: store i32 %{{.*}}, ptr %{{.*}}arena
// ASAN: call ptr @__llvm_arena_addr_v1
// ASAN: call void @__asan_report_load4
// ASAN: load i32, ptr %{{.*}}arena
