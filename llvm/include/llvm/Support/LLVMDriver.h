//===- LLVMDriver.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_LLVMDRIVER_H
#define LLVM_SUPPORT_LLVMDRIVER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorOr.h"

#include <functional>
#include <optional>

namespace llvm {
class ToolContext;
class CallableTool;

using ToolMainFn =
    std::function<int(ArrayRef<const char *>, const ToolContext &)>;

// This describes the calling context for a LLVM tool.
class ToolContext {
  // Possibly stores the two first command-line arguments which can be used to
  // re-invoke an LLVM tool. In regular, non-multicall build mode, only one
  // argument is stored. In llvm-driver mode (multicall) the first argument
  // would be the multicall binary path, and the second would be the tool name.
  SmallVector<const char *, 2> BinArgs;

  // Reference the module-local tools registry, which is used to allow tools to
  // call other tools in-process.
  void *State{};

public:
  // The path to the binary. This could be either:
  // - the invoked path, which might be a symlink: /path/to/clang++.exe
  // - the resolved path (realpath): /other/path/llvm.exe
  StringRef getPath() const { return BinArgs[0]; }

  // When dealing with llvm-driver builds, this is the second argument (the
  // tool). For example: /other/path/llvm.exe clang++
  StringRef getToolName() const;

  // Returns the first argument(s) required to re-invoke a binary.
  // It is usually just Argv[0] but in the context of multicall binaries, it
  // could be two arguments. For example '/path/to/llvm.exe clang-cl'.
  ArrayRef<const char *> invocationArgs() const { return BinArgs; }

  // This can be used to call other tools in-process. The arguments should
  // include the calling binary path.
  // Will return -1 if the tool (Args[0]) is not found.
  int callTool(ArrayRef<const char *> Args) const;

  // Find out if we have a tool linked in-process.
  // Takes a reference to a ArrayRef, which will be adjusted upon return if the
  // command-line is invoking a multicall binary, such as: `llvm clang++ ...`.
  // The returned ArrayRef will instead be `clang++ ...`.
  // Please note this function might require initialization of a
  // PDBSymbolDownloader prior to this call, if running on Windows and if
  // loading in-process of external LLVM binaries is desired.
  ErrorOr<CallableTool> getCallableTool(ArrayRef<const char *> &Args) const;

  // Same as above, but using the current context's invocation arguments.
  ErrorOr<CallableTool> getCallableTool() const;

  // Build a new ToolContext from this context and arguments.
  ToolContext build(ArrayRef<const char *> Args) const;

  // Whether a binary path is referencing a multicall binary.
  static bool isMulticallBinary(StringRef Path);

  // Build a root Toolcontext. This is a private function, do not use.
  static ToolContext buildRoot(void *State);
};

// Captures all the necessary information to allow invocation of a LLVM tool
// in-process.
class CallableTool {
public:
  // Call the tool in-process with an appropriate ToolContext.
  int call(const ToolContext &Ctx, ArrayRef<const char *> Args) const;

  operator bool() const { return MainFn != nullptr; }

  StringRef ToolName;
  ToolMainFn MainFn;
};

// A table of functions exported by binaries that support llvm-driver.
// This is only to be used by driver main function implementations.
struct ToolSymbolTable {
  using OnlyInitializeCRTFn = void (*)(void *InitEvent, void *ExitEvent);
  using MainCRTStartupFn = int(__cdecl *)(void *);
  using DynTLSInitFn = void(__cdecl *)(void *, unsigned long /*dwReason*/,
                                       void *);
  using DynTLSDtorFn = void(__cdecl *)(void *, unsigned long /*dwReason*/,
                                       void *);

  CallableTool Tool;
  OnlyInitializeCRTFn CRTInit;
  MainCRTStartupFn MainCRT;
  DynTLSInitFn TLSInit;
  DynTLSDtorFn TLSDtor;
};

// Implement a module-exported function meant to be retrieved dynamically when
// loading the binary (DLL or EXE) in-process.
#define LLVM_DRIVER_IMPL_MAIN(N, Fn)                                           \
  using namespace llvm;                                                        \
  int Fn(ArrayRef<const char *>, const ToolContext &);                         \
  /* In C:\Program Files\Microsoft Visual Studio\{vs_version}\{variant}\ */    \
  /* VC\Tools\MSVC\{msvc_version}\crt\src\vcruntime\exe_main.cpp */            \
  extern "C" int mainCRTStartup(void *);                                       \
  ToolSymbolTable LLVMDriverImplementTool(StringRef, ToolMainFn,               \
                                          ToolSymbolTable::MainCRTStartupFn);  \
  extern "C" LLVM_ALWAYS_EXPORT void getCallableTool(void *Out) {              \
    *static_cast<ToolSymbolTable *>(Out) =                                     \
        LLVMDriverImplementTool(N, Fn, &mainCRTStartup);                       \
  }                                                                            \
  int LLVMDriverCallMain(int, char **, StringRef, ToolMainFn MainFn);          \
  int main(int argc, char **argv) {                                            \
    return LLVMDriverCallMain(argc, argv, N, Fn);                              \
  }
} // namespace llvm

#endif
