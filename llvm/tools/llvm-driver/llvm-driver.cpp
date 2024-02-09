//===-- llvm-driver.cpp ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/LLVMDriver.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/WithColor.h"

using namespace llvm;

#define LLVM_DRIVER_TOOL(tool, entry)                                          \
  int entry##_main(int argc, char **argv, const llvm::ToolContext &);
#include "LLVMDriverTools.def"

constexpr char subcommands[] =
#define LLVM_DRIVER_TOOL(tool, entry) "  " tool "\n"
#include "LLVMDriverTools.def"
    ;

static ArrayRef<std::pair<StringRef, ToolContext::MainFn>> knownMainFns() {
  static constexpr std::pair<StringRef, ToolContext::MainFn> MainFns[] = {
#define LLVM_DRIVER_TOOL(tool, entry) {tool, entry##_main},
#include "LLVMDriverTools.def"
  };
  return MainFns;
}

static void printHelpMessage() {
  llvm::outs() << "OVERVIEW: llvm toolchain driver\n\n"
               << "USAGE: llvm [subcommand] [options]\n\n"
               << "SUBCOMMANDS:\n\n"
               << subcommands
               << "\n  Type \"llvm <subcommand> --help\" to get more help on a "
                  "specific subcommand\n\n"
               << "OPTIONS:\n\n  --help - Display this message";
}

static int findTool(ArrayRef<char *> Args, const ToolContext &TC) {
  // Create a specific context to understand if we are explicitly llvm.exe or if
  // we are a symlinked binary such as clang.exe.
  if (auto NewTC = TC.newContext(Args)) {
    int R = NewTC->callToolMain(Args);
    if (R != -1)
      return R;
  }
  printHelpMessage();
  // Return 0 if passed `--help` or return 1 otherwise.
  if (Args.size() > 1 && StringRef(Args[1]) == "--help")
    return 0;
  return 1;
}

int main(int Argc, char **Argv) {
  InitLLVM X(Argc, Argv);
  ToolContext::MainSymbol = (void *)(intptr_t)&knownMainFns;
  ToolContext TC{Argv[0], nullptr, /*NeedsPrependArg=*/false, /*Cleanup=*/false,
                 knownMainFns};
  return findTool(ArrayRef(Argv, Argc), TC);
}
