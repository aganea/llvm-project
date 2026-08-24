//===- diagtool_main.h - Entry point for invoking all diagnostic tools ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the main function for diagtool.
//
//===----------------------------------------------------------------------===//

#include "DiagTool.h"
#include "llvm/Support/LLVMDriver.h"

using namespace diagtool;

int diagtool_main(int argc, char **argv, const llvm::ToolContext &) {
  if (argc > 1)
    if (DiagTool *tool = diagTools->getTool(argv[1]))
      return tool->run(argc - 2, &argv[2], llvm::outs());

  llvm::errs() << "usage: diagtool <command> [<args>]\n\n";
  diagTools->printCommands(llvm::errs());
  return 1;    
}
