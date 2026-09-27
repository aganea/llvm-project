// COFF lifecycle IDs that share a grouped section need distinct, non-nested
// $a-$z ranges. Cover an ID without an underscore, one with a trailing
// underscore, and one with a nonempty key.
//
// REQUIRES: x86-registered-target
// RUN: split-file %s %t
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=foo -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/arena.cpp | FileCheck %s --check-prefix=PLAIN
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=foo_ -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/arena.cpp | FileCheck %s --check-prefix=TRAILING
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=foo_bar -fstatic-arena-list=%t/arena.list \
// RUN:   -o - %t/arena.cpp | FileCheck %s --check-prefix=KEYED

// PLAIN: @__cxx_init_fn_ptr = private constant ptr {{.*}}, section ".foo$$$u"
// TRAILING: @__cxx_init_fn_ptr = private constant ptr {{.*}}, section ".foo$$u"
// KEYED: @__cxx_init_fn_ptr = private constant ptr {{.*}}, section ".foo$bar$u"

//--- arena.list
[arena]
name:arena_dynamic

//--- arena.cpp
extern "C" int make_dynamic();
int arena_dynamic = make_dynamic();
