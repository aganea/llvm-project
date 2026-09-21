// The mandatory pre-optimization checker rejects the known cached-address
// shape, including casts/GEPs and values or destinations merged by phi/select.
//
// RUN: split-file %s %t
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/value.list \
// RUN:   -o /dev/null %t/rejected.cpp \
// RUN:   2>&1 | FileCheck %s --check-prefix=REJECT
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/all.list \
// RUN:   -o /dev/null %t/accepted.cpp

// REJECT-COUNT-3: rejected.cpp:1:5: error: arena address of arena_value is stored to non-arena global bad_cache
// REJECT: rejected.cpp:1:5: error: arena address of arena_value is stored to non-arena global bad_cache{{(_other)?}}

//--- value.list
[arena]
global:*arena_value*

//--- all.list
[arena]
global:*arena_*

//--- rejected.cpp
int arena_value;
int *bad_cache;
int *bad_cache_other;

void direct_escape() { bad_cache = &arena_value; }
void gep_escape() { bad_cache = &arena_value + 1; }
void select_escape(bool Which) {
  bad_cache = Which ? &arena_value : nullptr;
}

void merged_destination(bool Which) {
  *(Which ? &bad_cache_other : &bad_cache) = &arena_value;
}

//--- accepted.cpp
int arena_value;
int *arena_cache;
int *arena_cache_other;

void cache_inside_the_arena() { arena_cache = &arena_value; }
void merged_arena_destination(bool Which) {
  *(Which ? &arena_cache_other : &arena_cache) = &arena_value;
}
