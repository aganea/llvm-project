// Function-local arena objects use a relocated guard on both C++ ABIs. Weak
// namespace guards and every associated entity share the owner's COMDAT.
//
// RUN: split-file %s %t
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fexceptions -fcxx-exceptions -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/test.cpp \
// RUN:   | FileCheck %s --check-prefixes=COMMON,ITANIUM
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -std=c++20 -emit-llvm \
// RUN:   -fexceptions -fcxx-exceptions -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/test.cpp \
// RUN:   | FileCheck %s --check-prefixes=COMMON,MSVC
// RUN: %clang_cc1 -triple i386-pc-windows-msvc -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/test.cpp \
// RUN:   | FileCheck %s --check-prefix=MSVC32
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/vbase.list \
// RUN:   -o - %t/vbase.cpp \
// RUN:   | FileCheck %s --check-prefix=MSVC-VBASE
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -debug-info-kind=limited -dwarf-version=5 -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/test.cpp \
// RUN:   | FileCheck %s --check-prefix=DEBUG

//--- arena.list
[arena]
global:*arena_local*
global:*arena_inline*

//--- test.cpp
struct LocalObject {
  LocalObject();
  ~LocalObject();
  int Value;
};

int local_value() {
  static LocalObject arena_local;
  return arena_local.Value;
}

int constant_local_value() {
  static int arena_local_constant = 13;
  return arena_local_constant;
}

inline LocalObject arena_inline;
int inline_value() { return arena_inline.Value; }

// The local object and its ABI guard each have a record and a resolver call.
// ITANIUM-DAG: @__llvm_arena_var_v1._ZZ{{.*}}arena_local = internal externally_initialized global
// ITANIUM-DAG: @__llvm_arena_var_v1._ZGV{{.*}}arena_local = internal externally_initialized global
// MSVC-DAG: @"__llvm_arena_var_v1.{{.*}}arena_local{{.*}}" = internal externally_initialized global
// MSVC-DAG: @"__llvm_arena_var_v1.?$TSS{{.*}}local_value{{.*}}" = internal externally_initialized global
// COMMON-DAG: __llvm_arena_template_v1.{{.*}}arena_local_constant{{.*}} = internal constant i32 13

// The inline object's record is the COMDAT key. Its template/entry/lifecycle
// entities and Itanium namespace guard records join that same group.
// ITANIUM-DAG: $__llvm_arena_var_v1.arena_inline = comdat any
// ITANIUM-DAG: @__llvm_arena_var_v1.arena_inline = linkonce_odr {{.*}} comdat
// ITANIUM-DAG: @__llvm_arena_ptr_v1.arena_inline = private constant ptr @__llvm_arena_var_v1.arena_inline, section "llvma_v1", comdat($__llvm_arena_var_v1.arena_inline)
// ITANIUM-DAG: @__llvm_arena_var_v1._ZGV12arena_inline = linkonce_odr {{.*}} comdat($__llvm_arena_var_v1.arena_inline)
// ITANIUM-DAG: @__cxx_init_fn_ptr{{.*}} comdat($__llvm_arena_var_v1.arena_inline)

// COMMON-LABEL: define {{.*}}i32 {{.*}}local_value
// ITANIUM: [[GUARD:%.*]] = call ptr @__llvm_arena_addr_v1(ptr {{.*}}__llvm_arena_var_v1._ZGV{{.*}}arena_local)
// MSVC: [[GUARD:%.*]] = call ptr @__llvm_arena_addr_v1(ptr {{.*}}__llvm_arena_var_v1.?$TSS{{.*}}local_value{{.*}})
// COMMON: load {{.*}}, ptr [[GUARD]]

// The Itanium acquire, exceptional abort, and release paths all receive the
// resolved guard rather than the erased @_ZGV* placeholder.
// ITANIUM: call i32 @__cxa_guard_acquire(ptr [[GUARD]])
// ITANIUM: invoke void @_ZN11LocalObjectC1Ev(ptr {{.*}})
// ITANIUM: call i32 @__llvm_arena_atexit_v1
// ITANIUM: call void @__cxa_guard_release(ptr [[GUARD]])
// ITANIUM: call void @__cxa_guard_abort(ptr [[GUARD]])

// The Microsoft epoch implementation likewise uses the relocated address for
// both runtime calls and its unordered guard loads.
// MSVC: call void @_Init_thread_header(ptr [[GUARD]])
// MSVC: load atomic i32, ptr [[GUARD]] unordered
// MSVC: call void @_Init_thread_footer(ptr [[GUARD]])
// MSVC: call void @_Init_thread_abort(ptr [[GUARD]])

// On 32-bit MSVC the arena callback is cdecl and calls the real thiscall
// destructor with its declared convention.
// MSVC32-LABEL: define internal void {{.*}}__llvm_arena_dtor_adapter_v1.{{.*}}(ptr
// MSVC32: call x86_thiscallcc void {{.*}}LocalObject{{.*}}(ptr
// MSVC32: ret void

// Local arena storage deliberately has no debug-global description in v1.
// DEBUG-NOT: DIGlobalVariable(name: "arena_local"
// DEBUG-NOT: DIGlobalVariable(name: "arena_local_constant"

//--- vbase.list
[arena]
global:*arena_vbase*

//--- vbase.cpp
struct VirtualBase {
  virtual ~VirtualBase();
};

struct WithVirtualBase : virtual VirtualBase {
  WithVirtualBase();
  ~WithVirtualBase();
};

WithVirtualBase arena_vbase;

// The adapter is always void(void *), while normal CodeGen destruction emits
// the Microsoft complete-destructor call for a class with a virtual base.
// MSVC-VBASE-LABEL: define internal void {{.*}}__llvm_arena_dtor_adapter_v1.{{.*}}arena_vbase{{.*}}(ptr
// MSVC-VBASE-NOT: call void {{.*}}WithVirtualBase{{.*}}(ptr {{.*}},
// MSVC-VBASE: call void {{.*}}WithVirtualBase{{.*}}(ptr
// MSVC-VBASE: ret void
