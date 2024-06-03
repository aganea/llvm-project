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
#include "llvm/ADT/StringRef.h"

namespace llvm {
class ToolContext {
public:
  // The path to the binary/PE, which might include the current tool.
  // ie. `/path/to/llvm` or `C:\path\to\clang-cl.exe`
  StringRef BinaryPath;
  // The name of the current tool, if provided separately from the binary name.
  // ie. `llvm.exe clang++ ...`
  StringRef ProvidedToolName;
  // Whether the tool needs to cleanup the memory after execution.
  // A tool that runs a single time in a PE doesn't cleanup, to speed up
  // shutdown. When multiple tools are called in-process, we need to cleanup
  // after each execution.
  bool Cleanup = false;

  // The main function associated with the current verbatim tool.
  using MainFn = int (*)(int Argc, char **Argv, const llvm::ToolContext &TC);
  MainFn Main;

  // The verbatim tool name. For example if calling `i386-clang++-15`, the
  // verbatim name is `clang`.
  StringRef VerbatimToolName;

  // The name of the tool, including the target triple or the version number.
  StringRef getProgramName() const;

  // The arguments that would be used to re-invoke this tool.
  std::vector<const char *> executionArgs() const;

  // The stringified arguments that would be used to re-invoke this tool.
  std::string executionArgsString() const;

  // Invoke the current tool in-process. If the tool is not callable, returns -1
  // in the same as the `llvm::sys::ExecuteAndWait` API.
  int callToolMain(ArrayRef<const char *> Args) const;

  // Creates a new context from this current tool and bind it to a new tool, if
  // it exists.
  std::optional<ToolContext> newContext(ArrayRef<const char *> Args) const;

  void setCanonicalPrefixes(bool CanonicalPrefixes);

  // For compatibility reasons, until all code is migrated to using this class,
  // just return the initial main() Argv[0].
  StringRef getArgv0() const;

  using KnownToolsFn =
      ArrayRef<std::pair<StringRef, ToolContext::MainFn>> (*)();
  static void setTools(KnownToolsFn Tools, const char *Argv0 = nullptr);
};

// Identical to a ToolContext, except that the strings are internalized.
struct OwnerToolContext : public ToolContext {
  std::string Execution;
};
} // namespace llvm

#endif
