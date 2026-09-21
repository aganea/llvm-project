// Verify the core static-arena IR contract on both supported object formats.
//
// RUN: split-file %s %t
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/test.cpp \
// RUN:   | FileCheck %s --check-prefixes=CHECK,ELF
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/test.cpp \
// RUN:   | FileCheck %s --check-prefixes=CHECK,COFF
// RUN: %clang_cc1 -triple i386-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/test.cpp \
// RUN:   | FileCheck %s --check-prefix=ILP32
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -O2 -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/test.cpp \
// RUN:   | FileCheck %s --check-prefix=OPT
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -debug-info-kind=limited -dwarf-version=5 \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/test.cpp \
// RUN:   | FileCheck %s --check-prefix=DEBUG
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/test.cpp \
// RUN:   | FileCheck %s --check-prefix=O0
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/range.cpp \
// RUN:   | FileCheck %s --check-prefix=RANGE
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -o - %t/test.cpp | FileCheck %s --check-prefix=DISABLED \
// RUN:   --implicit-check-not=__llvm_arena_

//--- arena.list
[arena]
global:*arena_*

//--- test.cpp
extern "C" {
int process_target;
int arena_zero;
int arena_nonzero = 42;
int *arena_pointer = &process_target;

struct Aggregate {
  int X;
  long long Y;
};
Aggregate arena_aggregate = {7, 11};

struct Dynamic {
  Dynamic();
  ~Dynamic();
  int X;
};
Dynamic arena_dynamic;
}

int use_arena_globals() {
  arena_zero = arena_nonzero;
  return arena_zero + arena_aggregate.X + arena_dynamic.X + *arena_pointer;
}

// There is one mutable, externally-initialized record per selected definition.
// The offset is the fail-closed UINT64_MAX sentinel. Zero-initialized storage
// has no template, while nonzero scalar, aggregate, and process-global pointer
// representations do.
// CHECK-DAG: @__llvm_arena_var_v1.arena_zero = {{(dso_local )?}}externally_initialized global { i64, i64, i64, ptr, ptr } { i64 -1, i64 4, i64 4, ptr null,
// CHECK-DAG: @__llvm_arena_var_v1.arena_nonzero = {{(dso_local )?}}externally_initialized global { i64, i64, i64, ptr, ptr } { i64 -1, i64 4, i64 4, ptr @__llvm_arena_template_v1.arena_nonzero,
// CHECK-DAG: @__llvm_arena_template_v1.arena_nonzero = {{(dso_local )?}}constant i32 42, align 4
// CHECK-DAG: @__llvm_arena_template_v1.arena_pointer = {{(dso_local )?}}constant ptr @process_target
// CHECK-DAG: @__llvm_arena_template_v1.arena_aggregate = {{(dso_local )?}}constant %struct.Aggregate { i32 7, i64 11 }

// Every retained table member is the pointer entry, not merely its record.
// ELF-DAG: @__llvm_arena_ptr_v1.arena_zero = private constant ptr @__llvm_arena_var_v1.arena_zero, section "llvma_v1", align 8
// COFF-DAG: @__llvm_arena_ptr_v1.arena_zero = private constant ptr @__llvm_arena_var_v1.arena_zero, section ".llvma$v1$b", align 8
// ELF-DAG: @__cxx_init_fn_ptr = private constant ptr @__cxx_global_var_init, section "arena_test"
// COFF-DAG: @__cxx_init_fn_ptr = private constant ptr @"??__Earena_dynamic@@YAXXZ", section ".arena$test$u"
// CHECK: @llvm.used = appending global
// CHECK-SAME: ptr @__llvm_arena_ptr_v1.arena_zero

// Dynamic construction and destruction both use the resolver result. The
// lifecycle pointer is emitted whenever the selected object requires dynamic
// initialization.
// ELF-LABEL: define internal void @__cxx_global_var_init
// COFF-LABEL: define internal void @"??__Earena_dynamic@@YAXXZ"
// CHECK: [[DYN:%.*]] = call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1.arena_dynamic)
// CHECK: call {{.*}}Dynamic{{.*}}(ptr {{.*}}[[DYN]])
// CHECK: call i32 @__llvm_arena_atexit_v1(ptr @__llvm_arena_dtor_adapter_v1.arena_dynamic, ptr [[DYN]])

// Ordinary accesses are resolver calls and no original storage survives module
// finalization. Nothing is represented as fake TLS.
// CHECK-LABEL: define {{.*}}i32 {{.*}}use_arena_globals{{.*}}
// CHECK: call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1.arena_nonzero)
// CHECK: call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1.arena_zero)
// CHECK-NOT: @arena_zero =
// CHECK-NOT: thread_local
// CHECK: !{i32 1, !"static-arena-abi", i32 1}

// Fixed-width record fields remain i64 on a 32-bit target, while pointer-entry
// alignment follows the target pointer ABI.
// ILP32: @__llvm_arena_var_v1.arena_zero = externally_initialized global { i64, i64, i64, ptr, ptr }
// ILP32: @__llvm_arena_ptr_v1.arena_zero = private constant ptr @__llvm_arena_var_v1.arena_zero, section "llvma_v1", align 4

// Optimization must not erase or speculate the conservative resolver calls.
// OPT-DAG: declare ptr @__llvm_arena_addr_v1(ptr)
// OPT-LABEL: define {{.*}}i32 @_Z17use_arena_globalsv
// OPT-COUNT-6: call ptr @__llvm_arena_addr_v1
// OPT-NOT: memory(read)

// v1 deliberately emits no global-variable debug description for relocated
// namespace storage.
// DEBUG-NOT: DIGlobalVariable(name: "arena_zero"
// DEBUG-NOT: DIGlobalVariable(name: "arena_dynamic"

// The mandatory O0 finalization removes every selected storage placeholder;
// the module flag near EOF anchors these negative checks across the module.
// O0-NOT: @arena_zero =
// O0-NOT: @arena_nonzero =
// O0-NOT: @arena_dynamic =
// O0-NOT: thread_local
// O0: !{i32 1, !"static-arena-abi", i32 1}

// With the feature disabled, storage, startup and accesses retain their normal
// forms; there is no arena ABI surface in the module.
// DISABLED-DAG: @arena_zero = global i32 0, align 4
// DISABLED-DAG: @arena_nonzero = global i32 42, align 4
// DISABLED-DAG: @llvm.global_ctors = appending global
// DISABLED-LABEL: define {{.*}}i32 @_Z17use_arena_globalsv
// DISABLED: load i32, ptr @arena_nonzero
// DISABLED: store i32 {{.*}}, ptr @arena_zero

// A range-for's implicit reference is a non-ODR constant use in the AST, but
// its selected global base still has to retain the resolver-derived address.
// RANGE-LABEL: define {{.*}}i32 @_Z15sum_arena_rangev
// RANGE-COUNT-1: call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1.arena_range)
// RANGE: call {{.*}} @_ZN10ArenaRange5beginEv
// RANGE: call {{.*}} @_ZN10ArenaRange3endEv
// RANGE-NOT: @arena_range =

//--- range.cpp
struct ArenaRange {
  int *begin();
  int *end();
};

extern "C" {
ArenaRange arena_range;
}

int sum_arena_range() {
  int Sum = 0;
  for (int Value : arena_range)
    Sum += Value;
  return Sum;
}
