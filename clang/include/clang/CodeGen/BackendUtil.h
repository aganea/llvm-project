//===--- BackendUtil.h - LLVM Backend Utilities -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_CODEGEN_BACKENDUTIL_H
#define LLVM_CLANG_CODEGEN_BACKENDUTIL_H

#include "clang/Basic/LLVM.h"
#include "llvm/IR/ModuleSummaryIndex.h"
#include <future>
#include <memory>

namespace llvm {
class BitcodeModule;
template <typename T> class Expected;
template <typename T> class IntrusiveRefCntPtr;
class Module;
class MemoryBufferRef;
namespace vfs {
class FileSystem;
} // namespace vfs
} // namespace llvm

namespace clang {
class CompilerInstance;
class DiagnosticsEngine;
class CodeGenOptions;
class BackendConsumer;

enum BackendAction {
  Backend_EmitAssembly, ///< Emit native assembly files
  Backend_EmitBC,       ///< Emit LLVM bitcode files
  Backend_EmitLL,       ///< Emit human-readable LLVM assembly
  Backend_EmitNothing,  ///< Don't emit anything (benchmarking mode)
  Backend_EmitMCNull,   ///< Run CodeGen, but don't emit anything
  Backend_EmitObj       ///< Emit native object files
};

void emitBackendOutput(CompilerInstance &CI, CodeGenOptions &CGOpts,
                       StringRef TDesc, llvm::Module *M, BackendAction Action,
                       llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS,
                       std::unique_ptr<raw_pwrite_stream> OS,
                       BackendConsumer *BC = nullptr);

void EmbedBitcode(llvm::Module *M, const CodeGenOptions &CGOpts,
                  llvm::MemoryBufferRef Buf);

void EmbedDynamicDebugBitcode(llvm::Module *M, const CodeGenOptions &CGOpts,
                              StringRef OutputPath);

/// AOT mode: clone the module and run -O0 codegen to produce a .alt.obj.
/// Returns a future that must be waited on (valid only when parallel mode
/// is enabled; otherwise the codegen runs synchronously and the returned
/// future is empty).
std::future<void> EmitDynamicDebugAOT(llvm::Module *M, CompilerInstance &CI,
                                      const CodeGenOptions &CGOpts,
                                      StringRef OutputPath);

void EmbedObject(llvm::Module *M, const CodeGenOptions &CGOpts,
                 llvm::vfs::FileSystem &VFS, DiagnosticsEngine &Diags);
} // namespace clang

#endif
