// Constant initialization is only used as an arena template when the object is
// safe to bitwise-clone. Non-cloneable C++ objects are instead initialized and
// destroyed once per arena, at namespace lifecycle time or behind the
// function-local guard.
//
// RUN: split-file %s %t
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fexceptions -fcxx-exceptions -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%t/arena.list -o - %t/test.cpp \
// RUN:   | FileCheck %s --check-prefixes=COMMON,ELF \
// RUN:       --implicit-check-not=__llvm_arena_template_v1.arena_namespace \
// RUN:       --implicit-check-not=__llvm_arena_template_v1.arena_nonclone \
// RUN:       --implicit-check-not=__llvm_arena_template_v1.{{.*}}arena_local
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -std=c++20 -emit-llvm \
// RUN:   -fexceptions -fcxx-exceptions -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%t/arena.list -o - %t/test.cpp \
// RUN:   | FileCheck %s --check-prefixes=COMMON,COFF \
// RUN:       --implicit-check-not=__llvm_arena_template_v1.arena_namespace \
// RUN:       --implicit-check-not=__llvm_arena_template_v1.arena_nonclone \
// RUN:       --implicit-check-not=__llvm_arena_template_v1.{{.*}}arena_local
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -o - %t/test.cpp | FileCheck %s --check-prefix=DISABLED \
// RUN:       --implicit-check-not=__llvm_arena_

//--- arena.list
[arena]
global:*arena_*

//--- test.cpp
template <class T> struct UniquePtrLike {
  T *Pointer;
  constexpr UniquePtrLike() noexcept : Pointer(nullptr) {}
  UniquePtrLike(const UniquePtrLike &) = delete;
  ~UniquePtrLike();
};

struct NonClone {
  int Value;
  constexpr NonClone() noexcept : Value(7) {}
  NonClone(const NonClone &) {}
};

extern "C" {
UniquePtrLike<int> arena_namespace;
NonClone arena_nonclone;
}

UniquePtrLike<int> &arena_local_value() {
  static UniquePtrLike<int> arena_local;
  return arena_local;
}

// None of these records has a template: their initializer must execute anew
// for every arena, even though the C++ declarations are constant-initialized.
// COMMON-DAG: @__llvm_arena_var_v1.arena_namespace = {{(dso_local )?}}externally_initialized global { i64, i64, i64, ptr, ptr } { i64 -1, i64 8, i64 8, ptr null,
// COMMON-DAG: @__llvm_arena_var_v1.arena_nonclone = {{(dso_local )?}}externally_initialized global { i64, i64, i64, ptr, ptr } { i64 -1, i64 4, i64 4, ptr null,
// ELF-DAG: @__llvm_arena_var_v1.{{.*}}arena_local{{.*}} = internal externally_initialized global { i64, i64, i64, ptr, ptr } { i64 -1, i64 8, i64 8, ptr null,
// COFF-DAG: @"__llvm_arena_var_v1.{{.*}}arena_local{{.*}}" = internal externally_initialized global { i64, i64, i64, ptr, ptr } { i64 -1, i64 8, i64 8, ptr null,

// Namespace initialization is retained in the arena lifecycle on both object
// formats. It resolves the new storage, runs the constructor, and registers
// the destructor with the arena rather than the process.
// ELF-DAG: @__cxx_init_fn_ptr = private constant ptr @__cxx_global_var_init, section "arena_test"
// COFF-DAG: @__cxx_init_fn_ptr = private constant ptr @"??__Earena_namespace@@YAXXZ", section ".arena$test$u"
// ELF-LABEL: define internal void @__cxx_global_var_init()
// COFF-LABEL: define internal void @"??__Earena_namespace@@YAXXZ"()
// COMMON: [[NAMESPACE:%.*]] = call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1.arena_namespace)
// COMMON: call {{.*}}UniquePtrLike{{.*}}(ptr {{.*}}[[NAMESPACE]])
// COMMON: call i32 @__llvm_arena_atexit_v1(ptr @__llvm_arena_dtor_adapter_v1.arena_namespace, ptr [[NAMESPACE]])
// COMMON: ret void

// The non-cloneable object with a trivial destructor also gets a lifecycle
// initializer rather than a template copy.
// ELF-LABEL: define internal void @__cxx_global_var_init.1()
// COFF-LABEL: define internal void @"??__Earena_nonclone@@YAXXZ"()
// COMMON: [[NONCLONE:%.*]] = call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1.arena_nonclone)
// COMMON: call {{.*}}NonClone{{.*}}(ptr {{.*}}[[NONCLONE]])
// COMMON-NOT: __llvm_arena_atexit_v1
// COMMON: ret void

// A function-local object performs construction and arena destructor
// registration only after acquiring its relocated ABI guard.
// COMMON-LABEL: define {{.*}}ptr {{.*}}arena_local_value{{.*}}()
// COMMON: call ptr @__llvm_arena_addr_v1(ptr {{.*}}__llvm_arena_var_v1.{{.*}}arena_local{{.*}})
// ELF: [[GUARD:%.*]] = call ptr @__llvm_arena_addr_v1(ptr {{.*}}__llvm_arena_var_v1._ZGV{{.*}}arena_local)
// COFF: [[GUARD:%.*]] = call ptr @__llvm_arena_addr_v1(ptr {{.*}}__llvm_arena_var_v1.?$TSS{{.*}}arena_local_value{{.*}})
// ELF: call i32 @__cxa_guard_acquire(ptr [[GUARD]])
// COFF: call void @_Init_thread_header(ptr [[GUARD]])
// COMMON: [[LOCAL_INIT:%.*]] = call ptr @__llvm_arena_addr_v1(ptr {{.*}}__llvm_arena_var_v1.{{.*}}arena_local{{.*}})
// COMMON: call {{.*}}UniquePtrLike{{.*}}(ptr {{.*}}[[LOCAL_INIT]])
// COMMON: call i32 @__llvm_arena_atexit_v1(ptr {{.*}}, ptr [[LOCAL_INIT]])
// ELF: call void @__cxa_guard_release(ptr [[GUARD]])
// COFF: call void @_Init_thread_footer(ptr [[GUARD]])

// Without -fstatic-arena, Clang keeps its ordinary constant-storage and
// process-lifetime behavior.
// DISABLED-DAG: @arena_namespace = global {{.*}}zeroinitializer
// DISABLED-DAG: @arena_nonclone = global {{.*}}i32 7
