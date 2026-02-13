//===-- llvm-driver.cpp ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This file is only compiled when LLVM is built in multi-call driver mode.
// Supporting functions are in DriverUtils.cpp.
//===----------------------------------------------------------------------===//

#include "llvm/Support/LLVMDriver.h"

using namespace llvm;

// Forward-declare all tool entry points from LLVMDriverTools.def.
#define LLVM_DRIVER_TOOL(tool, entry)                                          \
  int entry##_main(ArrayRef<const char *>, const ToolContext &);
#include "LLVMDriverTools.def"

// Implemented in DriverUtils.cpp (not exposed in LLVMDriver.h).
int LLVMDriverMain(int Argc, char **Argv, ArrayRef<CallableTool> Tools);

int main(int Argc, char **Argv) {
  const CallableTool Tools[] = {
#define LLVM_DRIVER_TOOL(tool, entry) {tool, entry##_main},
#include "LLVMDriverTools.def"
  };
  return LLVMDriverMain(Argc, Argv, Tools);
}
