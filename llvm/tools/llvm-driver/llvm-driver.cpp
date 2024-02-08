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

static int findTool(int Argc, char **Argv, const ToolContext &TC) {
  if (!Argc || StringRef(Argv[0]) == "--help") {
    printHelpMessage();
    return (int)!Argc;
  }

  int R = TC.callToolMain(ArrayRef(Argv, Argc));
  if (R != -1)
    return R;

  printHelpMessage();
  return 1;
}

int main(int Argc, char **Argv) {
  InitLLVM X(Argc, Argv);
  ToolContext TC{Argv[0], nullptr, /*NeedsPrependArg=*/false, /*Cleanup=*/false,
                 knownMainFns};
  return findTool(Argc, Argv, TC);
}
