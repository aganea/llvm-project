//===- LLVMDriverTest.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Config/llvm-config.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Driver.h"
#include "llvm/Support/StaticArena.h"
#include "gtest/gtest.h"

#include <atomic>
#include <thread>

using namespace llvm;

namespace {
struct InvocationObservations {
  const void *LifecycleContext = nullptr;
  const void *MainContext = nullptr;
  const void *DestructorContext = nullptr;
  const void *LifecycleArena = nullptr;
  const void *MainArena = nullptr;
  const void *DestructorArena = nullptr;
  bool LifecycleHasArena = false;
  bool MainHasArena = false;
  bool DestructorHasArena = false;
};

#if defined(_WIN32) || defined(__ELF__) || defined(__wasm__)
#define LLVM_DRIVER_TEST_HAS_STATIC_ARENA 1
#else
#define LLVM_DRIVER_TEST_HAS_STATIC_ARENA 0
#endif

#if LLVM_DRIVER_TEST_HAS_STATIC_ARENA
static LLVMStaticArenaVarV1 InvocationObservationRecord = {
    UINT64_MAX, sizeof(InvocationObservations *),
    alignof(InvocationObservations *), nullptr,
    "LLVMDriverTest.InvocationObservation"};

#if defined(_MSC_VER)
#pragma section(".llvma$v1$b", read)
#if defined(_M_IX86)
#define LLVM_DRIVER_TEST_FORCE_INCLUDE(Name)                                   \
  __pragma(comment(linker, "/include:_" #Name))
#else
#define LLVM_DRIVER_TEST_FORCE_INCLUDE(Name)                                   \
  __pragma(comment(linker, "/include:" #Name))
#endif
#define LLVM_DRIVER_TEST_ARENA_ENTRY(Name, Record)                             \
  extern "C" __declspec(allocate(".llvma$v1$b"))                               \
  LLVMStaticArenaVarV1 *const Name = &(Record);                                \
  LLVM_DRIVER_TEST_FORCE_INCLUDE(Name)
#elif defined(_WIN32)
#define LLVM_DRIVER_TEST_ARENA_ENTRY(Name, Record)                             \
  extern "C" __attribute__((section(".llvma$v1$b"), used))                     \
  LLVMStaticArenaVarV1 *const Name = &(Record)
#elif defined(__ELF__) || defined(__wasm__)
#define LLVM_DRIVER_TEST_ARENA_ENTRY(Name, Record)                             \
  extern "C" LLVM_ATTRIBUTE_RETAIN __attribute__((section("llvma_v1"), used))  \
  LLVMStaticArenaVarV1 *const Name = &(Record)
#else
#define LLVM_DRIVER_TEST_ARENA_ENTRY(Name, Record)
#endif

LLVM_DRIVER_TEST_ARENA_ENTRY(LLVMDriverTestInvocationObservationEntry,
                             InvocationObservationRecord);

#undef LLVM_DRIVER_TEST_ARENA_ENTRY
#if defined(_MSC_VER)
#undef LLVM_DRIVER_TEST_FORCE_INCLUDE
#endif
#endif

thread_local InvocationObservations *CurrentObservations = nullptr;

#if LLVM_DRIVER_TEST_HAS_STATIC_ARENA
void observeDestructor(void *Opaque) {
  auto &Observations = **static_cast<InvocationObservations **>(Opaque);
  Observations.DestructorContext = cl::getCurrentContextIdentity();
  Observations.DestructorArena = getCurrentStaticArenaIdentity();
  Observations.DestructorHasArena = hasCurrentStaticArena();
}

void observeLifecycle() {
  ASSERT_NE(CurrentObservations, nullptr);
  CurrentObservations->LifecycleContext = cl::getCurrentContextIdentity();
  CurrentObservations->LifecycleArena = getCurrentStaticArenaIdentity();
  CurrentObservations->LifecycleHasArena = hasCurrentStaticArena();
  auto **DestructorObject = static_cast<InvocationObservations **>(
      __llvm_arena_addr_v1(&InvocationObservationRecord));
  *DestructorObject = CurrentObservations;
  ASSERT_EQ(__llvm_arena_atexit_v1(observeDestructor, DestructorObject), 0);
}
#endif

int observeMain(int, char **, const ToolContext &) {
  EXPECT_NE(CurrentObservations, nullptr);
  CurrentObservations->MainContext = cl::getCurrentContextIdentity();
  CurrentObservations->MainArena = getCurrentStaticArenaIdentity();
  CurrentObservations->MainHasArena = hasCurrentStaticArena();
  return 17;
}

TEST(LLVMDriverTest, ScopedInvocationOwnsLifecycleAndRestoresOuterState) {
#if !LLVM_DRIVER_TEST_HAS_STATIC_ARENA
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  InvocationObservations Observations;
  CurrentObservations = &Observations;
  const void *OuterContext = cl::getCurrentContextIdentity();
  ASSERT_FALSE(hasCurrentStaticArena());

  ToolContext Context{"llvm", nullptr, false};
  {
    ScopedToolInvocation Invocation(observeLifecycle);
    EXPECT_EQ(observeMain(0, nullptr, Context), 17);
  }

  EXPECT_TRUE(Observations.LifecycleHasArena);
  EXPECT_TRUE(Observations.MainHasArena);
  EXPECT_TRUE(Observations.DestructorHasArena);
  EXPECT_EQ(Observations.LifecycleContext, Observations.MainContext);
  EXPECT_EQ(Observations.LifecycleContext, Observations.DestructorContext);
  EXPECT_EQ(Observations.LifecycleArena, Observations.MainArena);
  EXPECT_EQ(Observations.LifecycleArena, Observations.DestructorArena);
  EXPECT_NE(Observations.MainContext, OuterContext);
  EXPECT_EQ(cl::getCurrentContextIdentity(), OuterContext);
  EXPECT_FALSE(hasCurrentStaticArena());
  CurrentObservations = nullptr;
#endif
}

#if LLVM_DRIVER_TEST_HAS_STATIC_ARENA
int runNestedMain(int, char **, const ToolContext &Context) {
  const void *OuterContext = cl::getCurrentContextIdentity();
  const void *OuterArena = getCurrentStaticArenaIdentity();
  EXPECT_TRUE(hasCurrentStaticArena());

  InvocationObservations Inner;
  InvocationObservations *OuterObservations = CurrentObservations;
  CurrentObservations = &Inner;
  EXPECT_EQ(
      runLLVMDriverTool(observeLifecycle, observeMain, 0, nullptr, Context),
      17);
  CurrentObservations = OuterObservations;
  EXPECT_NE(Inner.MainContext, OuterContext);
  EXPECT_NE(Inner.MainArena, OuterArena);
  EXPECT_EQ(cl::getCurrentContextIdentity(), OuterContext);
  EXPECT_EQ(getCurrentStaticArenaIdentity(), OuterArena);
  EXPECT_TRUE(hasCurrentStaticArena());
  return 23;
}
#endif

TEST(LLVMDriverTest, NestedEarlyReturnRestoresEnclosingInvocation) {
#if !LLVM_DRIVER_TEST_HAS_STATIC_ARENA
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  InvocationObservations Outer;
  CurrentObservations = &Outer;
  ToolContext Context{"llvm", nullptr, false};
  EXPECT_EQ(
      runLLVMDriverTool(observeLifecycle, runNestedMain, 0, nullptr, Context),
      23);
  EXPECT_FALSE(hasCurrentStaticArena());
  CurrentObservations = nullptr;
#endif
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

TEST(LLVMDriverTest, CommandLineOnlyInvocationMasksEnclosingArena) {
#if !LLVM_DRIVER_TEST_HAS_STATIC_ARENA
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  ScopedToolInvocation Outer(+[] {});
  const void *OuterContext = cl::getCurrentContextIdentity();
  const void *OuterArena = getCurrentStaticArenaIdentity();
  ASSERT_NE(OuterArena, nullptr);

  {
    ScopedToolInvocation CommandLineOnly;
    EXPECT_FALSE(hasCurrentStaticArena());
    EXPECT_EQ(getCurrentStaticArenaIdentity(), nullptr);
    EXPECT_NE(cl::getCurrentContextIdentity(), OuterContext);
  }

  EXPECT_EQ(cl::getCurrentContextIdentity(), OuterContext);
  EXPECT_EQ(getCurrentStaticArenaIdentity(), OuterArena);
#endif
}

#if LLVM_DRIVER_TEST_HAS_STATIC_ARENA && LLVM_ENABLE_THREADS
struct ConcurrentState {
  std::atomic<unsigned> *Ready;
  std::atomic<const void *> *ObservedContext;
  std::atomic<const void *> *ObservedArena;
};
thread_local ConcurrentState *CurrentConcurrentState = nullptr;

int observeConcurrentMain(int, char **, const ToolContext &) {
  EXPECT_TRUE(hasCurrentStaticArena());
  CurrentConcurrentState->ObservedContext->store(
      cl::getCurrentContextIdentity(), std::memory_order_relaxed);
  CurrentConcurrentState->ObservedArena->store(getCurrentStaticArenaIdentity(),
                                               std::memory_order_relaxed);
  CurrentConcurrentState->Ready->fetch_add(1, std::memory_order_release);
  while (CurrentConcurrentState->Ready->load(std::memory_order_acquire) != 2)
    std::this_thread::yield();
  return 0;
}
#endif

TEST(LLVMDriverTest, ConcurrentRootsHaveDistinctContexts) {
#if !LLVM_ENABLE_THREADS
  GTEST_SKIP() << "thread support is unavailable";
#elif !LLVM_DRIVER_TEST_HAS_STATIC_ARENA
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  std::atomic<unsigned> Ready{0};
  std::atomic<const void *> ContextA{nullptr};
  std::atomic<const void *> ContextB{nullptr};
  std::atomic<const void *> ArenaA{nullptr};
  std::atomic<const void *> ArenaB{nullptr};
  ToolContext Context{"llvm", nullptr, false};

  auto Run = [&](std::atomic<const void *> &ObservedContext,
                 std::atomic<const void *> &ObservedArena) {
    ConcurrentState State{&Ready, &ObservedContext, &ObservedArena};
    CurrentConcurrentState = &State;
    EXPECT_EQ(runLLVMDriverTool(
                  +[] {}, observeConcurrentMain, 0, nullptr, Context),
              0);
    CurrentConcurrentState = nullptr;
  };

  std::thread A([&] { Run(ContextA, ArenaA); });
  std::thread B([&] { Run(ContextB, ArenaB); });
  A.join();
  B.join();
  EXPECT_NE(ContextA.load(std::memory_order_relaxed), nullptr);
  EXPECT_NE(ContextB.load(std::memory_order_relaxed), nullptr);
  EXPECT_NE(ContextA.load(std::memory_order_relaxed),
            ContextB.load(std::memory_order_relaxed));
  EXPECT_NE(ArenaA.load(std::memory_order_relaxed), nullptr);
  EXPECT_NE(ArenaB.load(std::memory_order_relaxed), nullptr);
  EXPECT_NE(ArenaA.load(std::memory_order_relaxed),
            ArenaB.load(std::memory_order_relaxed));
#endif
}
#undef LLVM_DRIVER_TEST_HAS_STATIC_ARENA
} // namespace
