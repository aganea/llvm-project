// Verify declaration-only and coalescable static-arena storage identities.
//
// RUN: split-file %s %t
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/extern.cpp \
// RUN:   | FileCheck %s --check-prefix=EXTERN \
// RUN:     --implicit-check-not='@arena_external ='
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/extern.cpp \
// RUN:   | FileCheck %s --check-prefix=EXTERN \
// RUN:     --implicit-check-not='@arena_external ='
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++14 \
// RUN:   -Wno-static-float-init -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/available.cpp \
// RUN:   | FileCheck %s --check-prefix=AVAILABLE \
// RUN:     --implicit-check-not='@_ZN15AvailableMember12arena_memberE ='
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/weak.cpp \
// RUN:   | FileCheck %s --check-prefix=WEAK \
// RUN:     --implicit-check-not='@arena_weak ='
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/weak.cpp \
// RUN:   | FileCheck %s --check-prefix=WEAK-COFF
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -x c -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/flex.c \
// RUN:   | FileCheck %s --check-prefix=FLEX \
// RUN:     --implicit-check-not='@arena_flex ='
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -x c -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/tentative.c \
// RUN:   | FileCheck %s --check-prefix=TENTATIVE \
// RUN:     --implicit-check-not='@arena_tentative ='

//--- arena.list
[arena]
global:*arena_*

//--- extern.cpp
extern int arena_external;
int read_external() { return arena_external; }

// Keep this record's LLVM type opaque in a reference-only TU. Its explicit
// declaration alignment is nevertheless sufficient to form an arena address.
struct ExternalRecord {
  void touch();
};
extern ExternalRecord arena_external_record;
void touch_external_record() { arena_external_record.touch(); }

// A reference-only TU declares the universal record and resolves through it,
// but does not claim storage ownership with a template or table entry.
// EXTERN: __llvm_arena_var_v1.{{.*}}arena_external{{.*}} = external {{(dso_local )?}}externally_initialized global { i64, i64, i64, ptr, ptr }
// EXTERN-DAG: __llvm_arena_var_v1.{{.*}}arena_external_record{{.*}} = external {{(dso_local )?}}externally_initialized global { i64, i64, i64, ptr, ptr }
// EXTERN-NOT: __llvm_arena_template_v1.{{.*}}arena_external
// EXTERN-NOT: __llvm_arena_name_v1.{{.*}}arena_external
// EXTERN-NOT: __llvm_arena_ptr_v1.{{.*}}arena_external
// EXTERN-LABEL: define {{.*}}i32 {{.*}}read_external{{.*}}
// EXTERN: call ptr @__llvm_arena_addr_v1(ptr {{.*}}__llvm_arena_var_v1.{{.*}}arena_external{{.*}})
// EXTERN-LABEL: define {{.*}}void {{.*}}touch_external_record{{.*}}
// EXTERN: call ptr @__llvm_arena_addr_v1(ptr {{.*}}__llvm_arena_var_v1.{{.*}}arena_external_record{{.*}})

//--- available.cpp
struct AvailableMember {
  // Clang's static-float-init extension gives this declaration an
  // available_externally initializer for optimization without making it a
  // definition or a constant-expression-usable variable.
  static const double arena_member = 1.0;
};

const double *address_available_member() {
  return &AvailableMember::arena_member;
}

// An available_externally initializer on the temporary storage placeholder
// must not turn a declaration-only entity into an arena definition.
// AVAILABLE: @__llvm_arena_var_v1._ZN15AvailableMember12arena_memberE = external {{(dso_local )?}}externally_initialized global { i64, i64, i64, ptr, ptr }
// AVAILABLE-NOT: @__llvm_arena_template_v1.{{.*}}arena_member
// AVAILABLE-NOT: @__llvm_arena_name_v1.{{.*}}arena_member
// AVAILABLE-NOT: @__llvm_arena_ptr_v1.{{.*}}arena_member
// AVAILABLE-LABEL: define {{.*}}ptr @_Z24address_available_memberv
// AVAILABLE: call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1._ZN15AvailableMember12arena_memberE)

//--- weak.cpp
struct WeakObject {
  WeakObject();
  ~WeakObject();
  int Value;
};

WeakObject arena_weak __attribute__((weak));
int read_weak() { return arena_weak.Value; }

// Plain WeakAny storage has no source COMDAT, so the record becomes the
// synthesized key shared by the record, entry, guard, init and lifecycle.
// WEAK-DAG: $__llvm_arena_var_v1.arena_weak = comdat any
// WEAK-DAG: @__llvm_arena_var_v1.arena_weak = weak externally_initialized global {{.*}} comdat
// WEAK-DAG: @__llvm_arena_ptr_v1.arena_weak = private constant ptr @__llvm_arena_var_v1.arena_weak, section "llvma_v1", comdat($__llvm_arena_var_v1.arena_weak)
// WEAK-DAG: @__llvm_arena_var_v1._ZGV10arena_weak = weak {{.*}} comdat($__llvm_arena_var_v1.arena_weak)
// WEAK-DAG: @__cxx_init_fn_ptr{{.*}} section "arena_test", comdat($__llvm_arena_var_v1.arena_weak)
// WEAK-LABEL: define internal void @__cxx_global_var_init{{.*}} comdat($__llvm_arena_var_v1.arena_weak)
// WEAK: call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1._ZGV10arena_weak)
// WEAK: call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1.arena_weak)
// WEAK: call i32 @__llvm_arena_atexit_v1

// WEAK-COFF-DAG: $"__llvm_arena_var_v1.?arena_weak@@3UWeakObject@@A" = comdat any
// WEAK-COFF-DAG: @"__llvm_arena_var_v1.?arena_weak@@3UWeakObject@@A" = weak dso_local externally_initialized global {{.*}} comdat
// WEAK-COFF-DAG: @"__llvm_arena_ptr_v1.?arena_weak@@3UWeakObject@@A" = private constant ptr @"__llvm_arena_var_v1.?arena_weak@@3UWeakObject@@A", section ".llvma$v1$b", comdat($"__llvm_arena_var_v1.?arena_weak@@3UWeakObject@@A")
// WEAK-COFF-DAG: @__cxx_init_fn_ptr{{.*}} section ".arena$test$u", comdat($"__llvm_arena_var_v1.?arena_weak@@3UWeakObject@@A")
// WEAK-COFF-LABEL: define linkonce_odr dso_local void @"??__Earena_weak@@YAXXZ"() {{.*}} comdat($"__llvm_arena_var_v1.?arena_weak@@3UWeakObject@@A")
// WEAK-COFF: call ptr @__llvm_arena_addr_v1(ptr @"__llvm_arena_var_v1.?arena_weak@@3UWeakObject@@A")
// WEAK-COFF: call i32 @__llvm_arena_atexit_v1

//--- flex.c
struct Flex {
  int Count;
  char Data[];
};

struct Flex arena_flex = {3, {'a', 'b', 'c'}};
int read_flex(void) { return arena_flex.Data[2]; }

// The initializer replaces the provisional zero-tail storage type. Record size
// and template both use the final allocation, including its three-byte tail
// allocation rather than AST sizeof(struct Flex).
// FLEX-DAG: @__llvm_arena_var_v1.arena_flex = externally_initialized global { i64, i64, i64, ptr, ptr } { i64 -1, i64 7, i64 4, ptr @__llvm_arena_template_v1.arena_flex,
// FLEX-DAG: @__llvm_arena_template_v1.arena_flex = constant <{ i32, [3 x i8] }> <{ i32 3, [3 x i8] c"abc" }>, align 4
// FLEX-LABEL: define {{.*}}i32 @read_flex
// FLEX: call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1.arena_flex)

//--- tentative.c
int arena_tentative;
int read_tentative(void) { return arena_tentative; }

// Clang's default C mode is -fno-common, so a tentative definition owns one
// arena record. Explicit -fcommon remains a diagnosed v1 boundary.
// TENTATIVE-DAG: @__llvm_arena_var_v1.arena_tentative = {{(dso_local )?}}externally_initialized global { i64, i64, i64, ptr, ptr } { i64 -1, i64 4, i64 4, ptr null,
// TENTATIVE-DAG: @__llvm_arena_ptr_v1.arena_tentative = private constant ptr @__llvm_arena_var_v1.arena_tentative, section "llvma_v1", align 8
// TENTATIVE-LABEL: define {{.*}}i32 @read_tentative
// TENTATIVE: call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1.arena_tentative)
