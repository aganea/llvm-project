//===- LLVMDriver.cpp - Folded-tool invocation lifetime ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Driver.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/StaticArena.h"

#include <memory>

using namespace llvm;

namespace {
class ToolInvocationContext {
  // Declaration order is lifecycle order. Destruction reverses it: command
  // line state goes away while finalized arena storage is still bound, then
  // the outer arena binding is restored, then storage is released.
  std::unique_ptr<StaticArena> Arena;
  std::unique_ptr<ScopedStaticArenaBinding> ArenaBinding;
  cl::ScopedContext CommandLine;

public:
  explicit ToolInvocationContext(bool UsesPerInvocationGlobals)
      : Arena(UsesPerInvocationGlobals ? StaticArena::create() : nullptr),
        ArenaBinding(Arena ? std::make_unique<ScopedStaticArenaBinding>(*Arena)
                           : std::make_unique<ScopedStaticArenaBinding>(
                                 StaticArenaToken())) {}

  ~ToolInvocationContext() {
    // Combined close always takes the command-line side first, then arena.
    // Both checks fail closed if queued/running task leases remain.
    CommandLine.beginClosing();
    if (Arena) {
      Arena->beginClosing();
      Arena->runDestructors();
    }
  }
};
} // namespace

int llvm::runLLVMDriverTool(void (*InitLifecycle)(), LLVMDriverToolMain Main,
                            int Argc, char **Argv, const ToolContext &Context) {
  ToolInvocationContext Invocation(InitLifecycle != nullptr);
  if (InitLifecycle)
    InitLifecycle();
  return Main(Argc, Argv, Context);
}
