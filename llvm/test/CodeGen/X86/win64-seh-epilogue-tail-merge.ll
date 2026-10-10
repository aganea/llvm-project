; RUN: llc -O2 -mtriple=x86_64-pc-windows-msvc -mcpu=znver2 -filetype=obj -o /dev/null < %s
; RUN: llc -O2 -mtriple=x86_64-pc-windows-msvc -mcpu=znver2 < %s | FileCheck %s

; Branch folding must not tail-merge two Windows x64 epilogues by splitting a
; block between SEH_BeginEpilogue and SEH_EndEpilogue. Doing so made one
; epilogue jump into the middle of another, leaving a .seh_startepilogue
; without its .seh_endepilogue ("Missing .seh_endepilogue").

@g = external thread_local global ptr

define i1 @f(i32 %a, i32 %b, i32 %c, i64 %off, i1 %early, ptr %p) {
; CHECK-LABEL: f:
; CHECK:       .seh_endprologue
; CHECK:       .seh_startepilogue
; CHECK-NOT:   .seh_
; CHECK:       .seh_endepilogue
; CHECK:       .seh_startepilogue
; CHECK-NOT:   .seh_
; CHECK:       .seh_endepilogue
; CHECK:       .seh_startepilogue
; CHECK-NOT:   .seh_
; CHECK:       .seh_endepilogue
; CHECK-NOT:   .seh_startepilogue
; CHECK:       .seh_endproc
  br i1 %early, label %early.ret, label %check.a

early.ret:
  %a.zero = icmp eq i32 %a, 0
  ret i1 %a.zero

check.a:
  switch i32 %a, label %exit [
    i32 2659, label %load.p
    i32 1058, label %load.p
  ]

load.p:
  %v = load i32, ptr %p, align 8
  switch i32 %a, label %exit [
    i32 2659, label %load.g
    i32 1058, label %load.g
  ]

load.g:
  %base = load ptr, ptr @g, align 8
  %addr = getelementptr i8, ptr %base, i64 %off
  %byte = load i8, ptr %addr, align 8
  %flag = trunc i8 %byte to i1
  br i1 %flag, label %check.a.again, label %combine

combine:
  %v.bit = and i32 %v, 1
  %v.even = icmp eq i32 %v.bit, 0
  %c.zero = icmp eq i32 %c, 0
  %both = and i1 %v.even, %c.zero
  br label %exit

check.a.again:
  switch i32 %a, label %exit [
    i32 2659, label %check.b
    i32 1058, label %check.b
  ]

check.b:
  switch i32 %b, label %exit [
    i32 2659, label %false.ret
    i32 1058, label %false.ret
  ]

false.ret:
  ret i1 false

exit:
  %r = phi i1 [ true, %check.a ], [ %both, %combine ], [ true, %check.b ],
              [ true, %check.a.again ], [ true, %load.p ]
  ret i1 %r
}
