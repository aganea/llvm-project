//===-- DynDbgDeoptimizer.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// On-demand deoptimization engine: given a function name and a target, this
// class extracts pre-optimization bitcode, thins it to a single function,
// codegens at -O0, loads the resulting COFF .obj into the debuggee process,
// resolves relocations against the PDB symbol table, and patches the optimized
// function entry with a JMP to the unoptimized version.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_STRUCTUREDDATA_DYNDBG_DYNDBGDEOPTIMIZER_H
#define LLDB_SOURCE_PLUGINS_STRUCTUREDDATA_DYNDBG_DYNDBGDEOPTIMIZER_H

#include "lldb/Target/Target.h"
#include "lldb/Utility/Status.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lldb_private {

struct DynDbgBuildInfo {
  std::string WorkingDir;
  std::string CompilerPath;
  std::string SourceFile;
  std::string TypeServerPDB;
  std::string CommandLine;
  std::string ObjFilePath;
};

struct DynDbgPatchInfo {
  lldb::addr_t OriginalAddr = 0;
  lldb::addr_t UnoptimizedAddr = 0;
  std::string FunctionName;
  std::vector<uint8_t> OriginalBytes;
  lldb::addr_t AllocatedTextAddr = 0;
  size_t AllocatedTextSize = 0;
  lldb::addr_t AllocatedDataAddr = 0;
  size_t AllocatedDataSize = 0;
  lldb::ModuleSP JITModule;
};

class DynDbgDeoptimizer {
public:
  /// Get or create the deoptimizer for a given target.
  static DynDbgDeoptimizer &GetForTarget(Target &target);

  /// Deoptimize a single function: extract bitcode, thin, codegen, load, patch.
  /// Reports progress and errors to \p result_stream.
  Status Deoptimize(llvm::StringRef function_name, Stream &result_stream);

  /// Restore the optimized version of a previously deoptimized function.
  Status Reoptimize(llvm::StringRef function_name, Stream &result_stream);

  /// Restore all deoptimized functions.
  Status ReoptimizeAll(Stream &result_stream);

  /// Print status of all deoptimized functions.
  void PrintStatus(Stream &stream) const;

  bool IsDeoptimized(llvm::StringRef function_name) const;

private:
  /// Open the PDB for the module containing the function and extract
  /// LF_BUILDINFO for the compiland.
  Status GetBuildInfoForFunction(llvm::StringRef function_name,
                                 DynDbgBuildInfo &build_info,
                                 lldb::addr_t &function_addr);

  /// Extract the .dyndbg bitcode section from a COFF .obj, decompress it.
  static std::unique_ptr<llvm::MemoryBuffer>
  ExtractBitcodeFromObj(llvm::StringRef obj_path, std::string &error_msg);

  /// Lazy-load bitcode, materialize only the target function, externalize
  /// the rest, write thinned bitcode to output_path.
  /// \param[out] unopt_symbol_name  The mangled symbol name of the kept
  ///   function after renaming (e.g. "?MyFunc@@YAHXZ.dyndbg.unopt").
  static Status ThinBitcode(llvm::MemoryBuffer &bc_buf,
                            llvm::StringRef keep_function_name,
                            llvm::StringRef tu_hash,
                            llvm::StringRef output_path,
                            std::string &unopt_symbol_name,
                            Stream &result_stream);

  /// Invoke clang/llc to codegen the thinned bitcode at -O0.
  Status RunCodegen(const DynDbgBuildInfo &build_info,
                    llvm::StringRef thin_bc_path,
                    llvm::StringRef output_obj_path,
                    Stream &result_stream);

  /// Load a COFF .obj into the debuggee: allocate memory, copy sections,
  /// resolve relocations against the target's symbol table, return the
  /// address of the deoptimized function.
  Status LoadObjIntoDebuggee(llvm::StringRef obj_path,
                             llvm::StringRef unopt_function_name,
                             lldb::addr_t &unopt_addr,
                             DynDbgPatchInfo &patch_info,
                             Stream &result_stream);

  /// Resolve a symbol name to an address using the target's loaded modules.
  lldb::addr_t ResolveSymbolAddress(llvm::StringRef name);

  /// Write a JMP instruction at orig_addr that redirects to target_addr.
  Status PatchFunctionEntry(lldb::addr_t orig_addr, lldb::addr_t target_addr,
                            DynDbgPatchInfo &patch_info);

  /// Restore original bytes at a patched function entry.
  Status UnpatchFunctionEntry(const DynDbgPatchInfo &patch_info);

  /// Extract the TU hash from PDB public symbols (looks for .dyndbg.<hash>).
  std::string ExtractTUHash(llvm::StringRef pdb_path);

  /// Create a synthetic JIT Module with a symbol named
  /// "[Deoptimized] <function>" covering the loaded .text range, and register
  /// it with the target so that disassembly / backtraces show the label.
  void RegisterJITModule(DynDbgPatchInfo &patch_info);

  /// Remove a previously registered JIT Module from the target images.
  void UnregisterJITModule(DynDbgPatchInfo &patch_info);

  explicit DynDbgDeoptimizer(Target &target) : m_target(target) {}

  Target &m_target;
  llvm::StringMap<DynDbgPatchInfo> m_patches;

  static std::mutex s_instances_mutex;
  static llvm::DenseMap<Target *, std::unique_ptr<DynDbgDeoptimizer>>
      s_instances;
};

} // namespace lldb_private

#endif
