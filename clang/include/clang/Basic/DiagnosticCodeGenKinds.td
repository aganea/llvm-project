//==--- DiagnosticCodeGenKinds.td - codegen diagnostics -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Diagnostics emitted while lowering the AST to code. This component is shared
// between the LLVM IR code generator (clang/lib/CodeGen) and the ClangIR (CIR)
// pipeline (clang/lib/CIR). Existing LLVM IR codegen diagnostics still live in
// DiagnosticFrontendKinds.td and can migrate here incrementally.
//
//===----------------------------------------------------------------------===//

let Component = "CodeGen" in {

def err_static_arena_list : Error<"invalid static-arena list: %0">;

// Experimental per-invocation static-arena storage.
def err_static_arena_dll_storage : Error<
  "static-arena variable %0 cannot use dllimport or dllexport storage">;
def err_static_arena_constant_address : Error<
  "address of %0 is not a constant because its storage lives in the static "
  "arena; it is only known once an invocation is attached">;
def err_static_arena_alias : Error<
  "alias or weak reference to static-arena variable %0 is not supported">;
def err_static_arena_retained : Error<
  "static-arena variable %0 cannot be marked used or retained">;
def err_static_arena_unsupported_destructor : Error<
  "static-arena variable %0 has a destruction form unsupported by the v1 ABI">;
def err_static_arena_explicit_init_order : Error<
  "static-arena variable %0 cannot specify an explicit initialization order">;
def err_static_arena_requires_threadsafe_statics : Error<
  "static-arena local static %0 requires -fthreadsafe-statics on the Microsoft ABI">;
def err_static_arena_address_space : Error<
  "static-arena variable %0 must use the default address space">;
def err_static_arena_reference : Error<
  "reference variable %0 cannot be placed in the static arena">;
def err_static_arena_zero_size : Error<
  "zero-sized variable %0 cannot be placed in the static arena">;
def err_static_arena_template_static : Error<
  "static data member %0 of a template is not supported by the static-arena v1 ABI">;
def err_static_arena_threadprivate : Error<
  "OpenMP threadprivate variable %0 cannot be placed in the static arena">;
def err_static_arena_conflicting_storage : Error<
  "static-arena variable %0 has conflicting %1 storage">;
def err_static_arena_common : Error<
  "common variable %0 cannot be placed in the static arena; compile with -fno-common">;
def err_static_arena_undefined_weak : Error<
  "undefined weak variable %0 cannot be placed in the static arena">;
def err_static_arena_constant_object : Error<
  "constant-initialized static-arena variable %0 is not trivially copyable and bitwise cloneable">;
def err_static_arena_template_reference : Error<
  "static initializer template for %0 refers to static-arena variable %1">;
def err_static_arena_constexpr : Error<
  "constexpr or constant-expression-usable variable %0 cannot be placed in the static arena">;
def err_static_arena_direct_use : Error<
  "static-arena storage placeholder %0 still has a direct IR use">;
def err_static_arena_address_escape : Error<
  "arena address of %0 is stored to non-arena global %1">;

// ClangIR (CIR) diagnostics.

def err_cir_to_cir_transform_failed : Error<
    "CIR-to-CIR transformation failed">, DefaultFatal;

def err_cir_verification_failed_pre_passes : Error<
    "CIR module verification error before running CIR-to-CIR passes">,
    DefaultFatal;

// Diagnostics relayed from MLIR-side passes/verifier through the
// clang::DiagnosticsEngine. The full message text comes from MLIR.
def err_cir_mlir_diagnostic    : Error<"%0">;
def warn_cir_mlir_diagnostic   : Warning<"%0">, InGroup<ClangIR>;
def remark_cir_mlir_diagnostic : Remark<"%0">, InGroup<RemarkClangIR>;
def note_cir_mlir_diagnostic   : Note<"%0">;

}
