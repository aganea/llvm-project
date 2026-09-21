//===- LLVMDriverTest.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Driver.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/StaticArena.h"
#include "gtest/gtest.h"

#include <atomic>
#include <thread>

using namespace llvm;

namespace {
struct InvocationObservations {
  const void *LifecycleContext = nullptr;
  const void *MainContext = nullptr;
  bool LifecycleHasArena = false;
  bool MainHasArena = false;
};

thread_local InvocationObservations *CurrentObservations = nullptr;

void observeLifecycle() {
  ASSERT_NE(CurrentObservations, nullptr);
  CurrentObservations->LifecycleContext = cl::getCurrentContextIdentity();
  CurrentObservations->LifecycleHasArena = hasCurrentStaticArena();
}

int observeMain(int, char **, const ToolContext &) {
  EXPECT_NE(CurrentObservations, nullptr);
  CurrentObservations->MainContext = cl::getCurrentContextIdentity();
  CurrentObservations->MainHasArena = hasCurrentStaticArena();
  return 17;
}

TEST(LLVMDriverTest, InstallsLifecycleBeforeMainAndRestoresOuterState) {
  InvocationObservations Observations;
  CurrentObservations = &Observations;
  const void *OuterContext = cl::getCurrentContextIdentity();
  ASSERT_FALSE(hasCurrentStaticArena());

  ToolContext Context{"llvm", nullptr, false};
  EXPECT_EQ(
      runLLVMDriverTool(observeLifecycle, observeMain, 0, nullptr, Context),
      17);

  EXPECT_TRUE(Observations.LifecycleHasArena);
  EXPECT_TRUE(Observations.MainHasArena);
  EXPECT_EQ(Observations.LifecycleContext, Observations.MainContext);
  EXPECT_NE(Observations.MainContext, OuterContext);
  EXPECT_EQ(cl::getCurrentContextIdentity(), OuterContext);
  EXPECT_FALSE(hasCurrentStaticArena());
  CurrentObservations = nullptr;
}

int runNestedMain(int, char **, const ToolContext &Context) {
  const void *OuterContext = cl::getCurrentContextIdentity();
  EXPECT_TRUE(hasCurrentStaticArena());

  InvocationObservations Inner;
  InvocationObservations *OuterObservations = CurrentObservations;
  CurrentObservations = &Inner;
  EXPECT_EQ(
      runLLVMDriverTool(observeLifecycle, observeMain, 0, nullptr, Context),
      17);
  CurrentObservations = OuterObservations;
  EXPECT_NE(Inner.MainContext, OuterContext);
  EXPECT_EQ(cl::getCurrentContextIdentity(), OuterContext);
  EXPECT_TRUE(hasCurrentStaticArena());
  return 23;
}

TEST(LLVMDriverTest, NestedEarlyReturnRestoresEnclosingInvocation) {
  InvocationObservations Outer;
  CurrentObservations = &Outer;
  ToolContext Context{"llvm", nullptr, false};
  EXPECT_EQ(
      runLLVMDriverTool(observeLifecycle, runNestedMain, 0, nullptr, Context),
      23);
  EXPECT_FALSE(hasCurrentStaticArena());
  CurrentObservations = nullptr;
}

TEST(LLVMDriverTest, DeferredOnlyInvocationHasIndependentCommandLineContext) {
  InvocationObservations Observations;
  CurrentObservations = &Observations;
  const void *OuterContext = cl::getCurrentContextIdentity();
  ToolContext Context{"llvm", nullptr, false};

  EXPECT_EQ(runLLVMDriverTool(nullptr, observeMain, 0, nullptr, Context), 17);
  EXPECT_FALSE(Observations.MainHasArena);
  EXPECT_NE(Observations.MainContext, OuterContext);
  EXPECT_EQ(cl::getCurrentContextIdentity(), OuterContext);
  CurrentObservations = nullptr;
}

struct ConcurrentState {
  std::atomic<unsigned> *Ready;
  std::atomic<const void *> *Observed;
};
thread_local ConcurrentState *CurrentConcurrentState = nullptr;

int observeConcurrentMain(int, char **, const ToolContext &) {
  EXPECT_TRUE(hasCurrentStaticArena());
  CurrentConcurrentState->Observed->store(cl::getCurrentContextIdentity(),
                                          std::memory_order_relaxed);
  CurrentConcurrentState->Ready->fetch_add(1, std::memory_order_release);
  while (CurrentConcurrentState->Ready->load(std::memory_order_acquire) != 2)
    std::this_thread::yield();
  return 0;
}

TEST(LLVMDriverTest, ConcurrentRootsHaveDistinctContexts) {
  std::atomic<unsigned> Ready{0};
  std::atomic<const void *> ContextA{nullptr};
  std::atomic<const void *> ContextB{nullptr};
  ToolContext Context{"llvm", nullptr, false};

  auto Run = [&](std::atomic<const void *> &Observed) {
    ConcurrentState State{&Ready, &Observed};
    CurrentConcurrentState = &State;
    EXPECT_EQ(runLLVMDriverTool(
                  +[] {}, observeConcurrentMain, 0, nullptr, Context),
              0);
    CurrentConcurrentState = nullptr;
  };

  std::thread A([&] { Run(ContextA); });
  std::thread B([&] { Run(ContextB); });
  A.join();
  B.join();
  EXPECT_NE(ContextA.load(std::memory_order_relaxed), nullptr);
  EXPECT_NE(ContextB.load(std::memory_order_relaxed), nullptr);
  EXPECT_NE(ContextA.load(std::memory_order_relaxed),
            ContextB.load(std::memory_order_relaxed));
}
} // namespace
