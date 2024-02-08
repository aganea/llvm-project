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
  const char *Path{};
  const char *PrependArg{};
  // PrependArg will be added unconditionally by the llvm-driver, but
  // NeedsPrependArg will be false if Path is adequate to reinvoke the tool.
  // This is useful if realpath is ever called on Path, in which case it will
  // point to the llvm-driver executable, where PrependArg will be needed to
  // invoke the correct tool.
  bool NeedsPrependArg = false;
  // Whether the tool needs to cleanup the memory after execution.
  // A tool that runs a single time in a PE doesn't cleanup, to speed up
  // shutdown. When multiple tools are called in-process, we need to cleanup
  // after each execution.
  bool Cleanup = false;

  using MainFn = int (*)(int Argc, char **Argv, const llvm::ToolContext &TC);

  // A list of LLVM tools that live inside the current PE/binary. If the binary
  // embeds a single tool that isn't known to llvm-driver, this can empty or
  // nullptr.
  using KnownToolsFn = ArrayRef<std::pair<StringRef, MainFn>> (*)();
  KnownToolsFn KnownTools{};

  // Invoke the current tool in-process. If the tool is not callable, returns -1
  // in the same as the `llvm::sys::ExecuteAndWait` API.
  int callToolMain(ArrayRef<const char *> Args) const;

  // From a command line, check if a tool is present in-process.
  bool hasInProcessTool(ArrayRef<const char *> Args) const;

  // Creates a new contet from this current tool and bind it to a new tool, if
  // it exists.
  std::optional<ToolContext> newContext(StringRef Tool);
};
} // namespace llvm

#endif
