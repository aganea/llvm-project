; RUN: split-file %s %t

;; An unflagged module does not claim an arena ABI and can link with a flagged
;; module.  Two explicitly flagged modules must agree.
; RUN: llvm-link %t/none.ll %t/v1.ll -S -o - | FileCheck %s --check-prefix=V1
; RUN: llvm-link %t/v1.ll %t/none.ll -S -o - | FileCheck %s --check-prefix=V1
; RUN: llvm-link %t/v1.ll %t/v1.ll -S -o - | FileCheck %s --check-prefix=V1
; RUN: not llvm-link %t/v1.ll %t/v2.ll -S -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=CONFLICT

; V1: !{i32 1, !"static-arena-abi", i32 1}
; CONFLICT: linking module flags 'static-arena-abi': IDs have conflicting values

;--- none.ll
source_filename = "none"

;--- v1.ll
source_filename = "v1"

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"static-arena-abi", i32 1}

;--- v2.ll
source_filename = "v2"

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"static-arena-abi", i32 2}
