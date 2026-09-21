// Static-arena selection is fail-closed: an allowlisted declaration is either
// fully supported or diagnosed instead of silently retaining process storage.
//
// RUN: split-file %s %t
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm \
// RUN:   -fstatic-arena=arena_test %t/empty.cpp \
// RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=MISSING-LIST
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm \
// RUN:   -fstatic-arena=bad-id -fstatic-arena-list=%t/arena.list %t/empty.cpp \
// RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=INVALID-ID
// RUN: rm -f %t/unused-missing.list
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm \
// RUN:   -fstatic-arena-list=%t/unused-missing.list %t/empty.cpp \
// RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=UNUSED-MISSING-FILE \
// RUN:       --implicit-check-not="IO sandbox violation" \
// RUN:       --implicit-check-not="LLVM ERROR"
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/missing.list \
// RUN:   %t/empty.cpp -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=MISSING-FILE \
// RUN:       --implicit-check-not="IO sandbox violation" \
// RUN:       --implicit-check-not="LLVM ERROR"
// RUN: not %clang_cc1 -triple arm64-apple-darwin -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   %t/empty.cpp -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=FORMAT
// RUN: not %clang_cc1 -triple aarch64-unknown-linux-gnu -emit-llvm \
// RUN:   -fsanitize=hwaddress -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%t/arena.list \
// RUN:   %t/empty.cpp -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=HWASAN
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm \
// RUN:   -fopenmp -fopenmp-is-target-device -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%t/arena.list \
// RUN:   %t/empty.cpp -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=OMP-DEVICE
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm \
// RUN:   -fsycl-is-device -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%t/arena.list \
// RUN:   %t/empty.cpp -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=SYCL-DEVICE
// RUN: not %clang_cc1 -triple dxil-pc-shadermodel6.0-compute -x hlsl \
// RUN:   -emit-llvm -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   %t/empty.cpp -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=HLSL
// RUN: not %clang_cc1 -triple nvptx64-nvidia-cuda -x cuda \
// RUN:   -fcuda-is-device -emit-llvm -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%t/arena.list \
// RUN:   %t/empty.cpp -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CUDA-DEVICE
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   %t/empty.cpp -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CLANGIR
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -x ir -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   %t/empty.ll -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=LLVM-IR
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-tls.cpp \
// RUN:   -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-zero.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-constexpr.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-section.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-pragma-section.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-used.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-retained.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-loader.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-address-space.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -fopenmp \
// RUN:   -emit-llvm -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-threadprivate.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-reference.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-dynamic-reference.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-constant-address.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-dynamic-constant-address.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-priority.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -fms-extensions \
// RUN:   -std=c++20 -emit-llvm -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-init-seg.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-dtor.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-self-pointer.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-cross-pointer.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-cross-gep.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -x c -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-cross-ptrtoint.c -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-weak.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-weak-import.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-alias.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-weakref.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-weakref-src.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-template.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/rejected-named-register.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -x c -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/common-attribute.c -o /dev/null
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -x c -emit-llvm -fcommon \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/common.c -o /dev/null
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -fms-extensions -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/ms.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -fms-extensions \
// RUN:   -std=c++20 -emit-llvm -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/ms-export.cpp -o /dev/null
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -std=c++20 -emit-llvm \
// RUN:   -fno-threadsafe-statics -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%t/arena.list \
// RUN:   -verify %t/ms-local.cpp \
// RUN:   -o /dev/null

// MISSING-LIST: error: invalid argument '-fstatic-arena' only allowed with '-fstatic-arena-list=<file>'
// INVALID-ID: error: invalid value 'bad-id' in '-fstatic-arena='
// UNUSED-MISSING-FILE: error: invalid static-arena list: can't open file '{{.*}}unused-missing.list':
// MISSING-FILE: error: invalid static-arena list: can't open file '{{.*}}missing.list':
// FORMAT: error: unsupported option '-fstatic-arena' for target 'arm64-apple-darwin'
// HWASAN: error: invalid argument '-fstatic-arena' not allowed with '-fsanitize=hwaddress'
// OMP-DEVICE: error: invalid argument '-fstatic-arena' not allowed with 'OpenMP target device code'
// SYCL-DEVICE: error: invalid argument '-fstatic-arena' not allowed with 'SYCL device code'
// HLSL: error: invalid argument '-fstatic-arena' not allowed with 'HLSL'
// CUDA-DEVICE: error: invalid argument '-fstatic-arena' not allowed with 'CUDA device code'
// CLANGIR: error: invalid argument '-fstatic-arena' not allowed with 'ClangIR code generation'
// LLVM-IR: error: invalid argument '-fstatic-arena' not allowed with 'LLVM IR input'

//--- arena.list
[arena]
global:*bad_*
global:*rsp*
src:*selected-target.h

//--- empty.cpp

//--- empty.ll
source_filename = "empty"

// CodeGen stops after its first error, so each declaration-level rejection is
// a separate input. This also proves that every list match independently fails
// closed instead of relying on one earlier diagnostic.

//--- rejected-tls.cpp
thread_local int bad_tls; // expected-error {{conflicting thread-local storage}}

//--- rejected-zero.cpp
int bad_zero[0];          // expected-error {{zero-sized variable}}

//--- rejected-constexpr.cpp
constexpr int bad_constexpr = 1; // expected-error {{constexpr or constant-expression-usable}}
const int *use_constexpr() { return &bad_constexpr; }

//--- rejected-section.cpp
int bad_section __attribute__((section("bad"))); // expected-error {{conflicting section storage}}

//--- rejected-pragma-section.cpp
#pragma clang section data = ".bad"
int bad_pragma_section = 1; // expected-error {{conflicting section storage}}

//--- rejected-used.cpp
int bad_used __attribute__((used)); // expected-error {{cannot be marked used or retained}}

//--- rejected-retained.cpp
int bad_retained __attribute__((retain)); // expected-error {{cannot be marked used or retained}}

//--- rejected-loader.cpp
[[clang::loader_uninitialized]] int bad_loader; // expected-error {{conflicting loader-uninitialized storage}}

//--- rejected-address-space.cpp
int bad_address_space __attribute__((address_space(1))); // expected-error {{must use the default address space}}

//--- rejected-threadprivate.cpp
int bad_threadprivate; // expected-error {{OpenMP threadprivate variable}}
#pragma omp threadprivate(bad_threadprivate)

//--- rejected-reference.cpp
int process_storage;
int &bad_constant_reference = process_storage; // expected-error {{reference variable}}

//--- rejected-dynamic-reference.cpp
int &get_reference();
int &bad_dynamic_reference = get_reference(); // expected-error {{reference variable}}

//--- rejected-constant-address.cpp
int bad_address_target; // expected-error {{address of 'bad_address_target' is not a constant}}
int *ordinary_pointer = &bad_address_target;

//--- rejected-dynamic-constant-address.cpp
int make_value();
int bad_dynamic_address_target = // expected-error {{address of 'bad_dynamic_address_target' is not a constant}}
    make_value();
int *ordinary_dynamic_pointer = &bad_dynamic_address_target;

//--- rejected-priority.cpp
struct Dynamic {
  Dynamic();
};
__attribute__((init_priority(101))) Dynamic bad_priority; // expected-error {{cannot specify an explicit initialization order}}

//--- rejected-init-seg.cpp
#pragma init_seg(lib)
struct InitSeg {
  InitSeg();
};
InitSeg bad_init_seg; // expected-error {{cannot specify an explicit initialization order}}

//--- rejected-dtor.cpp
struct DtorElement {
  constexpr DtorElement() = default;
  ~DtorElement();
  int Value = 0;
};
DtorElement bad_dtor[2]; // expected-error {{destruction form unsupported}}

//--- rejected-self-pointer.cpp
void *bad_self_pointer = // expected-error {{static initializer template for bad_self_pointer refers to static-arena variable bad_self_pointer}}
    &bad_self_pointer;

//--- rejected-cross-pointer.cpp
int bad_cross_pointer_target;
int *bad_cross_pointer = // expected-error {{static initializer template for bad_cross_pointer refers to static-arena variable bad_cross_pointer_target}}
    &bad_cross_pointer_target;

//--- rejected-cross-gep.cpp
int bad_cross_gep_target[4];
int *bad_cross_gep = // expected-error {{static initializer template for bad_cross_gep refers to static-arena variable bad_cross_gep_target}}
    &bad_cross_gep_target[2];

//--- rejected-cross-ptrtoint.c
int bad_cross_ptrtoint_target;
__UINTPTR_TYPE__ bad_cross_ptrtoint = // expected-error {{static initializer template for bad_cross_ptrtoint refers to static-arena variable bad_cross_ptrtoint_target}}
    (__UINTPTR_TYPE__)&bad_cross_ptrtoint_target;

//--- rejected-weak.cpp
extern int bad_weak __attribute__((weak)); // expected-error {{undefined weak variable}}
int use_weak() { return bad_weak; }

//--- rejected-weak-import.cpp
extern int bad_weak_import __attribute__((weak_import)); // expected-error {{undefined weak variable}}
int use_weak_import() { return bad_weak_import; }

//--- rejected-alias.cpp
int ordinary;
extern int bad_alias __attribute__((alias("ordinary"))); // expected-error {{alias or weak reference}}

//--- rejected-weakref.cpp
static int ordinary_weakref // expected-error {{alias or weak reference}}
    __attribute__((weakref("bad_weakref_target")));
int use_weakref() { return ordinary_weakref; }

//--- selected-target.h
extern int ordinary_selected_target;

//--- rejected-weakref-src.cpp
#include "selected-target.h"
static int ordinary_header_weakref // expected-error {{alias or weak reference}}
    __attribute__((weakref("ordinary_selected_target")));
int use_header_weakref() { return ordinary_header_weakref; }

//--- rejected-template.cpp
template <class T> struct Holder {
  static int bad_template;
};
template <class T> int Holder<T>::bad_template; // expected-error {{static data member}}
template int Holder<int>::bad_template;

//--- rejected-named-register.cpp
register unsigned long bad_named_register asm("rsp"); // expected-error {{conflicting named-register storage}}
unsigned long use_named_register() { return bad_named_register; }

//--- common-attribute.c
int bad_common_attribute __attribute__((common)); // expected-error {{compile with -fno-common}}

//--- common.c
int bad_common; // expected-error {{compile with -fno-common}}

//--- ms.cpp
__declspec(dllimport) extern int bad_dll; // expected-error {{cannot use dllimport or dllexport storage}}
int use_dll() { return bad_dll; }

//--- ms-export.cpp
__declspec(dllexport) int bad_export; // expected-error {{cannot use dllimport or dllexport storage}}

//--- ms-local.cpp
struct Local {
  Local();
};
int f() {
  static Local bad_local; // expected-error {{requires -fthreadsafe-statics}}
  return 0;
}
