//===- LLVMDriver.cpp - Folded-tool invocation lifetime ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Driver.h"
#include "llvm/Support/StaticArena.h"

#include <memory>

using namespace llvm;

struct ScopedToolInvocation::Impl {
  // Declaration order is lifecycle order. Destruction reverses it: command
  // line state goes away while finalized arena storage is still bound, then
  // the outer arena binding is restored, then storage is released.
  std::unique_ptr<StaticArena> Arena;
  std::unique_ptr<ScopedStaticArenaBinding> ArenaBinding;
  cl::ScopedContext CommandLine;

  explicit Impl(bool UsesPerInvocationGlobals)
      : Arena(UsesPerInvocationGlobals ? StaticArena::create() : nullptr),
        ArenaBinding(Arena ? std::make_unique<ScopedStaticArenaBinding>(*Arena)
                           : std::make_unique<ScopedStaticArenaBinding>(
                                 StaticArenaToken())) {}

  ~Impl() {
    // Combined close always takes the command-line side first, then arena.
    // Both checks fail closed if queued/running task leases remain.
    CommandLine.beginClosing();
    if (Arena) {
      Arena->beginClosing();
      Arena->runDestructors();
    }
  }
};

ScopedToolInvocation::ScopedToolInvocation(
    ToolLifecycleInitializer InitLifecycle)
    : PImpl(std::make_unique<Impl>(InitLifecycle != nullptr)) {
  // Run only after PImpl is fully installed. Besides making the public order
  // explicit, this ensures an unwinding callback destroys the complete owner.
  if (InitLifecycle)
    InitLifecycle();
}

ScopedToolInvocation::~ScopedToolInvocation() = default;

int llvm::runLLVMDriverTool(ToolLifecycleInitializer InitLifecycle,
                            LLVMDriverToolMain Main, int Argc, char **Argv,
                            const ToolContext &Context) {
  ScopedToolInvocation Invocation(InitLifecycle);
  return Main(Argc, Argv, Context);
}
