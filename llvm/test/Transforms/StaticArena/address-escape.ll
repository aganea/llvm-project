; RUN: not opt -passes=static-arena-address-escape -disable-output %s 2>&1 \
; RUN:   | FileCheck %s
; RUN: %python %S/Inputs/deep-address-escape.py 4096 > %t.deep.ll
; RUN: not opt -passes=static-arena-address-escape -disable-output \
; RUN:   %t.deep.ll 2>&1 | FileCheck %s --check-prefix=DEEP

@__llvm_arena_var_v1.arena_value = external global i8
@__llvm_arena_var_v1.arena_cache = external global i8
@__llvm_arena_var_v1.arena_cache_other = external global i8

@bad_cache = global ptr null
@bad_cache_other = global ptr null
@bad_cache_array = global [1 x ptr] zeroinitializer
@bad_alias = alias ptr, ptr @bad_cache
@arena_cache = global ptr null #0

declare ptr @__llvm_arena_addr_v1(ptr)
declare void @unknown(ptr)

define void @direct() {
  %arena = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_value)
  store ptr %arena, ptr @bad_cache
  ret void
}

define void @gep() {
  %arena = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_value)
  %next = getelementptr i8, ptr %arena, i64 1
  store ptr %next, ptr @bad_cache
  ret void
}

define void @source_select(i1 %which) {
  %arena = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_value)
  %value = select i1 %which, ptr %arena, ptr null
  store ptr %value, ptr @bad_cache
  ret void
}

define void @source_phi(i1 %which) {
entry:
  %arena = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_value)
  br i1 %which, label %left, label %right

left:
  br label %merge

right:
  br label %merge

merge:
  %value = phi ptr [ %arena, %left ], [ null, %right ]
  store ptr %value, ptr @bad_cache
  ret void
}

; Cyclic SSA must not hide an arena root or make the analysis recurse forever.
define void @source_cycle(i1 %again) {
entry:
  %arena = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_value)
  br label %loop

loop:
  %value = phi ptr [ %arena, %entry ], [ %value, %loop ]
  store ptr %value, ptr @bad_cache
  br i1 %again, label %loop, label %exit

exit:
  ret void
}

; A cyclic destination cannot be proven invocation-relative. The underlying
; process-global path must still be diagnosed.
define void @destination_cycle(i1 %again) {
entry:
  %arena = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_value)
  br label %loop

loop:
  %destination = phi ptr [ %destination, %loop ], [ @bad_cache, %entry ]
  store ptr %arena, ptr %destination
  br i1 %again, label %loop, label %exit

exit:
  ret void
}

define void @destination_select(i1 %which) {
  %arena = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_value)
  %destination = select i1 %which, ptr @bad_cache, ptr @bad_cache_other
  store ptr %arena, ptr %destination
  ret void
}

define void @constant_gep_destination() {
  %arena = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_value)
  store ptr %arena, ptr getelementptr inbounds (
      [1 x ptr], ptr @bad_cache_array, i64 0, i64 0)
  ret void
}

define void @destination_alias() {
  %arena = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_value)
  store ptr %arena, ptr @bad_alias
  ret void
}

; All destination paths are invocation-relative, so the mixed resolver roots
; are safe even though they refer to different arena records.
define void @accepted_select(i1 %which) {
  %arena = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_value)
  %cache = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_cache)
  %other = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_cache_other)
  %destination = select i1 %which, ptr %cache, ptr %other
  store ptr %arena, ptr %destination
  ret void
}

; Reusing a proven subgraph must not look like a cycle.
define void @accepted_shared_destination(i1 %which) {
  %arena = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_value)
  %cache = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_cache)
  %shared = getelementptr i8, ptr %cache, i64 1
  %destination = select i1 %which, ptr %shared, ptr %shared
  store ptr %arena, ptr %destination
  ret void
}

define void @accepted_phi(i1 %which) {
entry:
  %arena = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_value)
  br i1 %which, label %left, label %right

left:
  %cache = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_cache)
  br label %merge

right:
  %other = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_cache_other)
  br label %merge

merge:
  %destination = phi ptr [ %cache, %left ], [ %other, %right ]
  store ptr %arena, ptr %destination
  ret void
}

; A selected placeholder and unknown non-global destinations are outside the
; rejected cached-global shape.
define void @accepted_placeholder_and_unknown() {
  %slot = alloca ptr
  %arena = call ptr @__llvm_arena_addr_v1(
      ptr @__llvm_arena_var_v1.arena_value)
  store ptr %arena, ptr @arena_cache
  store ptr %arena, ptr %slot
  call void @unknown(ptr %arena)
  ret void
}

attributes #0 = { "static-arena" }

; CHECK-COUNT-6: error: arena address of arena_value is stored to non-arena global bad_cache
; CHECK: error: arena address of arena_value is stored to non-arena global bad_cache{{(_other)?}}
; CHECK: error: arena address of arena_value is stored to non-arena global bad_cache_array
; CHECK: error: arena address of arena_value is stored to non-arena global bad_alias
; CHECK-NOT: error:

; DEEP: error: arena address of <unknown> is stored to non-arena global bad_cache
; DEEP: error: arena address of arena_value is stored to non-arena global <unknown>
; DEEP: error: arena address of arena_value is stored to non-arena global <unknown>
