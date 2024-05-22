//===- LLVMDriver.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_LLVMDRIVER_H
#define LLVM_SUPPORT_LLVMDRIVER_H

#include "llvm/Support/Compiler.h"

namespace llvm {
class raw_fd_ostream;

class ToolContext {
public:
  // A context for the current Tool used in this thread.
  static LLVM_THREAD_LOCAL ToolContext *Current;

  ToolContext() : LastContext(Current) { Current = this; }
  ~ToolContext() { Current = LastContext; }

  static ToolContext createSingleTool(const char *BinaryPath) {
    ToolContext TC;
    TC.Path = BinaryPath;
    return TC;
  }

  // Replace any previous thread-local context with a new one.
  static void setThreadContext(ToolContext *Ctx) { Current = Ctx; }

  const char *Path{};
  const char *PrependArg{};
  // PrependArg will be added unconditionally by the llvm-driver, but
  // NeedsPrependArg will be false if Path is adequate to reinvoke the tool.
  // This is useful if realpath is ever called on Path, in which case it will
  // point to the llvm-driver executable, where PrependArg will be needed to
  // invoke the correct tool.
  bool NeedsPrependArg = false;

  // Overrides for standard streams.
  raw_fd_ostream *OutsOverride{};
  raw_fd_ostream *ErrsOverride{};

  // The past Tool context used before this current one came into use.
  ToolContext *LastContext;
};

} // namespace llvm

#endif
