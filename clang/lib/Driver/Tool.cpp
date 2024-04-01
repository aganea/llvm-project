//===--- Tool.cpp - Compilation Tools -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Driver/Tool.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/InputInfo.h"
#include "clang/Driver/Job.h"
#include "llvm/Support/LLVMDriver.h"

using namespace clang::driver;

Tool::Tool(const char *_Name, const char *_ShortName, const ToolChain &TC)
    : Name(_Name), ShortName(_ShortName), TheToolChain(TC) {}

Tool::~Tool() {
}

void Tool::ConstructJobMultipleOutputs(Compilation &C, const JobAction &JA,
                                       const InputInfoList &Outputs,
                                       const InputInfoList &Inputs,
                                       const llvm::opt::ArgList &TCArgs,
                                       const char *LinkingOutput) const {
  assert(Outputs.size() == 1 && "Expected only one output by default!");
  ConstructJob(C, JA, Outputs.front(), Inputs, TCArgs, LinkingOutput);
}

void Tool::ConstructCommand(Compilation &C, ResponseFileSupport ResponseSupport,
                            const char *Executable, const JobAction &JA,
                            const InputInfo &Output,
                            const InputInfoList &Inputs,
                            const llvm::opt::ArgStringList &Args,
                            std::optional<llvm::ToolContext> ToolCtx,
    llvm::ArrayRef<const char *> NewEnvironment) const {
  std::unique_ptr<clang::driver::Command> LinkCmd;
  const Driver &D = C.getDriver();
  if (D.InProcess && !D.CCGenDiagnostics) {
    LinkCmd = std::make_unique<InProcessCommand>(
        JA, *this, ResponseSupport, Executable, Args, Inputs, Output, *ToolCtx);
  } else {
    LinkCmd = std::make_unique<Command>(JA, *this, ResponseSupport, Executable,
                                        Args, Inputs, Output);
    if (!NewEnvironment.empty())
      LinkCmd->setEnvironment(NewEnvironment);
  }
  C.addCommand(std::move(LinkCmd));
}
