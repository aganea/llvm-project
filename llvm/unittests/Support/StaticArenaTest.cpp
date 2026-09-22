//===- StaticArenaTest.cpp - Static-arena runtime tests -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/StaticArena.h"

#include "llvm/Config/llvm-config.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/ToolExecutionContext.h"
#if defined(_WIN32)
#include "llvm/Support/Windows/WindowsSupport.h"
#endif
#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <thread>

using namespace llvm;

namespace llvm {

class ToolExecutionContextTestPeer {
public:
  static bool tryCapture(ToolExecutionContext &Result) {
    return ToolExecutionContext::tryCapture(Result);
  }
};

} // namespace llvm

namespace {

static const uint32_t TemplateValue = 42;

static LLVMStaticArenaVarV1 TemplateRecord = {UINT64_MAX, sizeof(uint32_t),
                                              alignof(uint32_t), &TemplateValue,
                                              "StaticArenaTest.Template"};
static LLVMStaticArenaVarV1 OverAlignedRecord = {UINT64_MAX, 7, 64, nullptr,
                                                 "StaticArenaTest.OverAligned"};
static LLVMStaticArenaVarV1 CallbackObjectRecord = {
    UINT64_MAX, 32, 1, nullptr, "StaticArenaTest.CallbackObject"};
static LLVMStaticArenaVarV1 OptionRecord = {
    UINT64_MAX, sizeof(cl::opt<int, true>), alignof(cl::opt<int, true>),
    nullptr, "StaticArenaTest.Option"};
static LLVMStaticArenaVarV1 OptionLocationRecord = {
    UINT64_MAX, sizeof(int), alignof(int), nullptr,
    "StaticArenaTest.OptionLocation"};

static std::array<std::atomic<unsigned>, 2> LazyConstructionCounts;

template <unsigned Index> struct LazyContextValue {
  LazyContextValue() {
    LazyConstructionCounts[Index].fetch_add(1, std::memory_order_relaxed);
  }
};

static ContextManagedStatic<LazyContextValue<0>> FirstLazyContextValue;
static ContextManagedStatic<LazyContextValue<1>> SecondLazyContextValue;

} // namespace

#if defined(_MSC_VER)
#pragma section(".llvma$v1$b", read)
#if defined(_M_IX86)
#define STATIC_ARENA_TEST_FORCE_INCLUDE(Name)                                  \
  __pragma(comment(linker, "/include:_" #Name))
#else
#define STATIC_ARENA_TEST_FORCE_INCLUDE(Name)                                  \
  __pragma(comment(linker, "/include:" #Name))
#endif
#define STATIC_ARENA_TEST_ENTRY(Name, Record)                                  \
  extern "C" __declspec(allocate(".llvma$v1$b"))                               \
  LLVMStaticArenaVarV1 *const Name = &(Record);                                \
  STATIC_ARENA_TEST_FORCE_INCLUDE(Name)
#elif defined(_WIN32)
#define STATIC_ARENA_TEST_ENTRY(Name, Record)                                  \
  extern "C" __attribute__((section(".llvma$v1$b"), used))                     \
  LLVMStaticArenaVarV1 *const Name = &(Record)
#elif defined(__ELF__) || defined(__wasm__)
#define STATIC_ARENA_TEST_ENTRY(Name, Record)                                  \
  extern "C" LLVM_ATTRIBUTE_RETAIN __attribute__((section("llvma_v1"), used))  \
  LLVMStaticArenaVarV1 *const Name = &(Record)
#else
#define STATIC_ARENA_TEST_ENTRY(Name, Record)
#endif

STATIC_ARENA_TEST_ENTRY(LLVMStaticArenaTestTemplateEntry, TemplateRecord);
STATIC_ARENA_TEST_ENTRY(LLVMStaticArenaTestOverAlignedEntry, OverAlignedRecord);
STATIC_ARENA_TEST_ENTRY(LLVMStaticArenaTestCallbackEntry, CallbackObjectRecord);
STATIC_ARENA_TEST_ENTRY(LLVMStaticArenaTestOptionEntry, OptionRecord);
STATIC_ARENA_TEST_ENTRY(LLVMStaticArenaTestOptionLocationEntry,
                        OptionLocationRecord);

#undef STATIC_ARENA_TEST_ENTRY
#if defined(_MSC_VER)
#undef STATIC_ARENA_TEST_FORCE_INCLUDE
#endif

namespace {

static void finishArena(StaticArena &Arena) {
  Arena.beginClosing();
  Arena.runDestructors();
}

#if defined(_WIN32)
struct FiberContextTestState {
  void *DispatcherFiber = nullptr;
  unsigned Visits = 0;
  bool Finished = false;
};

static VOID WINAPI runFiberContextTest(void *OpaqueState) {
  auto &State = *static_cast<FiberContextTestState *>(OpaqueState);
  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  {
    ScopedStaticArenaBinding ArenaOwner(*Arena);
    cl::ScopedContext CommandLineOwner;
    void *ExpectedArenaAddress = __llvm_arena_addr_v1(&TemplateRecord);
    const void *ExpectedCommandLineContext = cl::getCurrentContextIdentity();

    for (unsigned I = 0; I != 2; ++I) {
      EXPECT_EQ(ExpectedArenaAddress, __llvm_arena_addr_v1(&TemplateRecord));
      EXPECT_EQ(ExpectedCommandLineContext, cl::getCurrentContextIdentity());
      ++State.Visits;

      // Mask the owner bindings while the neutral dispatcher runs. This scope
      // intentionally spans the switch on the worker fiber's stack; returning
      // from SwitchToFiber destroys it and restores the owner bindings.
      ScopedToolExecutionContext HideOwner{ToolExecutionContext()};
      EXPECT_FALSE(hasCurrentStaticArena());
      EXPECT_NE(ExpectedCommandLineContext, cl::getCurrentContextIdentity());
      SwitchToFiber(State.DispatcherFiber);
    }

    CommandLineOwner.beginClosing();
    finishArena(*Arena);
  }
  Arena.reset();

  State.Finished = true;
  SwitchToFiber(State.DispatcherFiber);
  ADD_FAILURE() << "the completed test fiber was resumed again";
  SwitchToFiber(State.DispatcherFiber);
}
#endif

TEST(StaticArenaTest, ConcurrentFirstCreationPublishesOneLayout) {
#if !LLVM_ENABLE_THREADS
  GTEST_SKIP() << "thread support is unavailable";
#elif !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  std::unique_ptr<StaticArena> ArenaA;
  std::unique_ptr<StaticArena> ArenaB;
  std::atomic<unsigned> Ready{0};
  std::atomic<bool> Create{false};

  auto Creator = [&](std::unique_ptr<StaticArena> *Result) {
    Ready.fetch_add(1, std::memory_order_release);
    while (!Create.load(std::memory_order_acquire))
      std::this_thread::yield();
    *Result = StaticArena::create();
  };

  std::thread ThreadA(Creator, &ArenaA);
  std::thread ThreadB(Creator, &ArenaB);
  while (Ready.load(std::memory_order_acquire) != 2)
    std::this_thread::yield();
  Create.store(true, std::memory_order_release);
  ThreadA.join();
  ThreadB.join();

  ASSERT_NE(nullptr, ArenaA);
  ASSERT_NE(nullptr, ArenaB);
  EXPECT_NE(ArenaA.get(), ArenaB.get());
  {
    ScopedStaticArenaBinding Binding(*ArenaA);
    EXPECT_EQ(42u,
              *static_cast<uint32_t *>(__llvm_arena_addr_v1(&TemplateRecord)));
    finishArena(*ArenaA);
  }
  {
    ScopedStaticArenaBinding Binding(*ArenaB);
    EXPECT_EQ(42u,
              *static_cast<uint32_t *>(__llvm_arena_addr_v1(&TemplateRecord)));
    finishArena(*ArenaB);
  }
#endif
}

TEST(StaticArenaTest, IndependentStorageTemplateAlignmentAndMembership) {
#if !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  EXPECT_FALSE(hasCurrentStaticArena());
  EXPECT_FALSE(isInCurrentStaticArena(nullptr));
  EXPECT_FALSE(isInCurrentStaticArena(&TemplateValue));
  std::unique_ptr<StaticArena> A = StaticArena::create();
  std::unique_ptr<StaticArena> B = StaticArena::create();
  uint32_t *AddressA = nullptr;
  uint32_t *AddressB = nullptr;

  {
    ScopedStaticArenaBinding Binding(*A);
    EXPECT_TRUE(hasCurrentStaticArena());
    AddressA = static_cast<uint32_t *>(__llvm_arena_addr_v1(&TemplateRecord));
    EXPECT_EQ(42u, *AddressA);
    *AddressA = 1;

    void *OverAligned = __llvm_arena_addr_v1(&OverAlignedRecord);
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(OverAligned) % 64);
    EXPECT_TRUE(isInCurrentStaticArena(AddressA));
    EXPECT_TRUE(isInCurrentStaticArena(static_cast<char *>(OverAligned) +
                                       OverAlignedRecord.Size - 1));
    EXPECT_FALSE(isInCurrentStaticArena(nullptr));
    int ProcessOwned = 0;
    EXPECT_FALSE(isInCurrentStaticArena(&ProcessOwned));

    // Membership deliberately covers the complete laid-out span, including
    // internal alignment holes, but excludes allocation tail padding.
    char *Storage = reinterpret_cast<char *>(AddressA) - TemplateRecord.Offset;
    LLVMStaticArenaVarV1 *Records[] = {&TemplateRecord, &OverAlignedRecord,
                                       &CallbackObjectRecord, &OptionRecord,
                                       &OptionLocationRecord};
    uint64_t ObjectSpan = 0;
    for (LLVMStaticArenaVarV1 *Record : Records)
      ObjectSpan = std::max(ObjectSpan, Record->Offset + Record->Size);
    ASSERT_NE(0u, ObjectSpan);
    EXPECT_TRUE(isInCurrentStaticArena(Storage + ObjectSpan - 1));
    EXPECT_FALSE(isInCurrentStaticArena(Storage + ObjectSpan));

    finishArena(*A);
  }

  EXPECT_FALSE(hasCurrentStaticArena());

  {
    ScopedStaticArenaBinding Binding(*B);
    AddressB = static_cast<uint32_t *>(__llvm_arena_addr_v1(&TemplateRecord));
    EXPECT_EQ(42u, *AddressB);
    *AddressB = 2;
    finishArena(*B);
  }

  EXPECT_NE(AddressA, AddressB);
  EXPECT_FALSE(isInCurrentStaticArena(AddressA));
  EXPECT_FALSE(isInCurrentStaticArena(AddressB));
#endif
}

TEST(StaticArenaTest, NestedBindingsRestoreTheOuterArena) {
#if !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  std::unique_ptr<StaticArena> ArenaA = StaticArena::create();
  std::unique_ptr<StaticArena> ArenaB = StaticArena::create();
  {
    ScopedStaticArenaBinding BindingA(*ArenaA);
    void *AddressA = __llvm_arena_addr_v1(&TemplateRecord);
    EXPECT_TRUE(isInCurrentStaticArena(AddressA));

    {
      ScopedStaticArenaBinding BindingB(*ArenaB);
      void *AddressB = __llvm_arena_addr_v1(&TemplateRecord);
      EXPECT_NE(AddressA, AddressB);
      EXPECT_FALSE(isInCurrentStaticArena(AddressA));
      EXPECT_TRUE(isInCurrentStaticArena(AddressB));
      finishArena(*ArenaB);
    }

    EXPECT_EQ(AddressA, __llvm_arena_addr_v1(&TemplateRecord));
    EXPECT_TRUE(isInCurrentStaticArena(AddressA));
    finishArena(*ArenaA);
  }
#endif
}

TEST(StaticArenaTest, EmptyTaskBindingMasksTheOuterArena) {
#if !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  {
    ScopedStaticArenaBinding OwnerBinding(*Arena);
    void *Address = __llvm_arena_addr_v1(&TemplateRecord);
    EXPECT_TRUE(isInCurrentStaticArena(Address));
    {
      ScopedStaticArenaBinding NoArena{StaticArenaToken()};
      EXPECT_FALSE(isInCurrentStaticArena(Address));
    }
    EXPECT_TRUE(isInCurrentStaticArena(Address));
    finishArena(*Arena);
  }
#endif
}

TEST(StaticArenaTest, TwoWorkersCanBindTheSameArenaConcurrently) {
#if !LLVM_ENABLE_THREADS
  GTEST_SKIP() << "thread support is unavailable";
#elif !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  {
    ScopedStaticArenaBinding OwnerBinding(*Arena);
    StaticArenaToken TokenA = StaticArenaToken::capture();
    StaticArenaToken TokenB = TokenA;
    std::atomic<unsigned> Ready{0};
    std::atomic<bool> ReleaseWorkers{false};
    std::atomic<void *> AddressA{nullptr};
    std::atomic<void *> AddressB{nullptr};
    std::atomic<bool> MembershipA{false};
    std::atomic<bool> MembershipB{false};

    auto Worker = [&](StaticArenaToken Token, std::atomic<void *> *Address,
                      std::atomic<bool> *Membership) {
      ScopedStaticArenaBinding Binding(std::move(Token));
      void *Object = __llvm_arena_addr_v1(&TemplateRecord);
      Address->store(Object, std::memory_order_relaxed);
      Membership->store(isInCurrentStaticArena(Object),
                        std::memory_order_relaxed);
      Ready.fetch_add(1, std::memory_order_release);
      while (!ReleaseWorkers.load(std::memory_order_acquire))
        std::this_thread::yield();
    };

    std::thread WorkerA(Worker, std::move(TokenA), &AddressA, &MembershipA);
    std::thread WorkerB(Worker, std::move(TokenB), &AddressB, &MembershipB);
    while (Ready.load(std::memory_order_acquire) != 2)
      std::this_thread::yield();
    ReleaseWorkers.store(true, std::memory_order_release);
    WorkerA.join();
    WorkerB.join();

    EXPECT_EQ(AddressA.load(std::memory_order_relaxed),
              AddressB.load(std::memory_order_relaxed));
    EXPECT_TRUE(MembershipA.load(std::memory_order_relaxed));
    EXPECT_TRUE(MembershipB.load(std::memory_order_relaxed));
    finishArena(*Arena);
  }
#endif
}

static std::array<std::atomic<unsigned>, 32> ConcurrentCallbackCounts;

static void countConcurrentCallback(void *Object) {
  auto Index = *static_cast<unsigned char *>(Object);
  ASSERT_LT(Index, ConcurrentCallbackCounts.size());
  ConcurrentCallbackCounts[Index].fetch_add(1, std::memory_order_relaxed);
}

TEST(StaticArenaTest, ConcurrentDestructorRegistrationRunsEveryCallback) {
#if !LLVM_ENABLE_THREADS
  GTEST_SKIP() << "thread support is unavailable";
#elif !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  for (std::atomic<unsigned> &Count : ConcurrentCallbackCounts)
    Count.store(0, std::memory_order_relaxed);

  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  {
    ScopedStaticArenaBinding OwnerBinding(*Arena);
    auto *Objects = static_cast<unsigned char *>(
        __llvm_arena_addr_v1(&CallbackObjectRecord));
    ASSERT_EQ(32u, CallbackObjectRecord.Size);
    ASSERT_TRUE(isInCurrentStaticArena(Objects));
    ASSERT_TRUE(isInCurrentStaticArena(Objects + 31));
    for (size_t I = 0; I < ConcurrentCallbackCounts.size(); ++I)
      Objects[I] = static_cast<unsigned char>(I);

    StaticArenaToken TokenA = StaticArenaToken::capture();
    StaticArenaToken TokenB = TokenA;
    std::atomic<unsigned> Ready{0};
    std::atomic<bool> Register{false};
    std::atomic<bool> Succeeded{true};
    auto Worker = [&](StaticArenaToken Token, unsigned Parity) {
      ScopedStaticArenaBinding Binding(std::move(Token));
      Ready.fetch_add(1, std::memory_order_release);
      while (!Register.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (size_t I = Parity; I < ConcurrentCallbackCounts.size(); I += 2) {
        if (!isInCurrentStaticArena(Objects + I)) {
          Succeeded.store(false, std::memory_order_relaxed);
          continue;
        }
        if (__llvm_arena_atexit_v1(countConcurrentCallback, Objects + I) != 0)
          Succeeded.store(false, std::memory_order_relaxed);
      }
    };

    std::thread WorkerA(Worker, std::move(TokenA), 0);
    std::thread WorkerB(Worker, std::move(TokenB), 1);
    while (Ready.load(std::memory_order_acquire) != 2)
      std::this_thread::yield();
    Register.store(true, std::memory_order_release);
    WorkerA.join();
    WorkerB.join();

    EXPECT_TRUE(Succeeded.load(std::memory_order_relaxed));
    finishArena(*Arena);
  }

  for (const std::atomic<unsigned> &Count : ConcurrentCallbackCounts)
    EXPECT_EQ(1u, Count.load(std::memory_order_relaxed));
#endif
}

static std::atomic<unsigned> CallbackOrder{0};

static void appendCallbackDigit(unsigned Digit) {
  unsigned Old = CallbackOrder.load(std::memory_order_relaxed);
  while (!CallbackOrder.compare_exchange_weak(Old, Old * 10 + Digit,
                                              std::memory_order_relaxed)) {
  }
}

static void callbackOne(void *) { appendCallbackDigit(1); }
static void callbackThree(void *) { appendCallbackDigit(3); }
static void callbackTwoAndRegisterAnother(void *Object) {
  appendCallbackDigit(2);
  EXPECT_EQ(0, __llvm_arena_atexit_v1(callbackThree, Object));
}

TEST(StaticArenaTest, FinalizationDrainsCallbacksRegisteredByCallbacks) {
#if !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  CallbackOrder.store(0, std::memory_order_relaxed);
  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  {
    ScopedStaticArenaBinding Binding(*Arena);
    void *Object = __llvm_arena_addr_v1(&CallbackObjectRecord);
    ASSERT_EQ(0, __llvm_arena_atexit_v1(callbackOne, Object));
    ASSERT_EQ(0, __llvm_arena_atexit_v1(callbackTwoAndRegisterAnother, Object));
    finishArena(*Arena);
  }
  EXPECT_EQ(231u, CallbackOrder.load(std::memory_order_relaxed));
#endif
}

static void destroyArenaOption(void *Object) {
  static_cast<cl::opt<int, true> *>(Object)->~opt();
}

using ArenaOption = cl::opt<int, true>;

TEST(StaticArenaTest, RejectsArenaOptionWithProcessLocation) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#elif !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  EXPECT_DEATH(
      {
        std::unique_ptr<StaticArena> Arena = StaticArena::create();
        ScopedStaticArenaBinding ArenaBinding(*Arena);
        cl::ScopedContext CommandLineContext;
        int ProcessLocation = 0;
        (void)::new (__llvm_arena_addr_v1(&OptionRecord)) ArenaOption(
            "arena-option-process-location", cl::location(ProcessLocation));
      },
      "cl::location storage must have the same invocation lifetime");
#endif
}

TEST(StaticArenaTest, ToolExecutionContextPropagatesArenaAndCommandLine) {
#if !LLVM_ENABLE_THREADS
  GTEST_SKIP() << "thread support is unavailable";
#elif !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  {
    ScopedStaticArenaBinding ArenaBinding(*Arena);
    {
      cl::ScopedContext CommandLineContext;
      int *Location =
          ::new (__llvm_arena_addr_v1(&OptionLocationRecord)) int(17);
      auto *Option = ::new (__llvm_arena_addr_v1(&OptionRecord))
          cl::opt<int, true>("arena-option", cl::location(*Location));
      ASSERT_EQ(0, __llvm_arena_atexit_v1(destroyArenaOption, Option));

      DefaultThreadPool Pool(hardware_concurrency(2));
      std::shared_future<int> Future = Pool.async([Option] {
        EXPECT_TRUE(isInCurrentStaticArena(Option));
        EXPECT_EQ(Option, cl::getRegisteredOptions().lookup("arena-option"));
        return static_cast<int>(*Option);
      });
      EXPECT_EQ(17, Future.get());
      Pool.wait();

      // Keeping a completed shared_future must not keep either task lease.
      CommandLineContext.beginClosing();
      finishArena(*Arena);
      EXPECT_EQ(0u, cl::getRegisteredOptions().count("arena-option"));
    }
  }
#endif
}

TEST(StaticArenaTest, ConcurrentWorkersConstructContextStaticsOnce) {
#if !LLVM_ENABLE_THREADS
  GTEST_SKIP() << "thread support is unavailable";
#elif !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  for (std::atomic<unsigned> &Count : LazyConstructionCounts)
    Count.store(0, std::memory_order_relaxed);

  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  {
    ScopedStaticArenaBinding ArenaBinding(*Arena);
    cl::ScopedContext CommandLineContext;

    std::array<std::atomic<unsigned>, 2> Ready{};
    std::array<std::atomic<unsigned>, 2> Done{};
    std::array<std::array<const void *, 2>, 2> Addresses{};
    ToolExecutionContext FirstContext = ToolExecutionContext::capture();
    ToolExecutionContext SecondContext = FirstContext;

    auto Worker = [&](unsigned WorkerIndex, ToolExecutionContext Context) {
      ScopedToolExecutionContext Binding(std::move(Context));
      for (unsigned Round = 0; Round != 2; ++Round) {
        Ready[Round].fetch_add(1, std::memory_order_release);
        while (Ready[Round].load(std::memory_order_acquire) != 2)
          std::this_thread::yield();

        Addresses[WorkerIndex][Round] =
            Round == 0 ? static_cast<const void *>(&*FirstLazyContextValue)
                       : static_cast<const void *>(&*SecondLazyContextValue);

        Done[Round].fetch_add(1, std::memory_order_release);
        while (Done[Round].load(std::memory_order_acquire) != 2)
          std::this_thread::yield();
      }
    };

    std::thread FirstWorker(Worker, 0, std::move(FirstContext));
    std::thread SecondWorker(Worker, 1, std::move(SecondContext));
    FirstWorker.join();
    SecondWorker.join();

    EXPECT_EQ(Addresses[0][0], Addresses[1][0]);
    EXPECT_EQ(Addresses[0][1], Addresses[1][1]);
    EXPECT_NE(Addresses[0][0], Addresses[0][1]);
    EXPECT_EQ(LazyConstructionCounts[0].load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(LazyConstructionCounts[1].load(std::memory_order_relaxed), 1u);

    CommandLineContext.beginClosing();
    finishArena(*Arena);
  }
#endif
}

TEST(StaticArenaTest, ResumableSlicesRestoreTheDispatcherContext) {
#if !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  std::unique_ptr<StaticArena> WorkArena = StaticArena::create();
  std::unique_ptr<StaticArena> DispatcherArena = StaticArena::create();
  ToolExecutionContext SuspendedWork;

  {
    ScopedStaticArenaBinding WorkOwner(*WorkArena);
    cl::ScopedContext WorkCommandLine;
    void *WorkAddress = __llvm_arena_addr_v1(&TemplateRecord);
    const void *WorkCommandLineIdentity = cl::getCurrentContextIdentity();
    SuspendedWork = ToolExecutionContext::capture();

    {
      ScopedStaticArenaBinding DispatcherOwner(*DispatcherArena);
      cl::ScopedContext DispatcherCommandLine;
      void *DispatcherAddress = __llvm_arena_addr_v1(&TemplateRecord);
      const void *DispatcherCommandLineIdentity =
          cl::getCurrentContextIdentity();
      EXPECT_NE(WorkAddress, DispatcherAddress);
      EXPECT_NE(WorkCommandLineIdentity, DispatcherCommandLineIdentity);

      // A coroutine scheduler uses this exact shape around handle.resume().
      // The binding is on the dispatcher stack, so a suspension returns
      // through its destructor and restores the dispatcher's context.
      auto ResumeOneSlice = [&] {
        ScopedToolExecutionContext Binding(SuspendedWork);
        EXPECT_EQ(WorkAddress, __llvm_arena_addr_v1(&TemplateRecord));
        EXPECT_EQ(WorkCommandLineIdentity, cl::getCurrentContextIdentity());
      };

      ResumeOneSlice();
      EXPECT_EQ(DispatcherAddress, __llvm_arena_addr_v1(&TemplateRecord));
      EXPECT_EQ(DispatcherCommandLineIdentity, cl::getCurrentContextIdentity());
      ResumeOneSlice();
      EXPECT_EQ(DispatcherAddress, __llvm_arena_addr_v1(&TemplateRecord));
      EXPECT_EQ(DispatcherCommandLineIdentity, cl::getCurrentContextIdentity());

      SuspendedWork = ToolExecutionContext();
      DispatcherCommandLine.beginClosing();
      finishArena(*DispatcherArena);
    }

    WorkCommandLine.beginClosing();
    finishArena(*WorkArena);
  }
#endif
}

TEST(StaticArenaTest, WindowsFiberMasksOwnerContextWhileSuspended) {
#if !defined(_WIN32)
  GTEST_SKIP() << "Win32 fibers are unavailable";
#elif LLVM_ADDRESS_SANITIZER_BUILD
  GTEST_SKIP() << "raw fiber switches need AddressSanitizer fiber hooks";
#else
  const bool ConvertedThread = !IsThreadAFiber();
  void *DispatcherFiber =
      ConvertedThread ? ConvertThreadToFiber(nullptr) : GetCurrentFiber();
  if (!DispatcherFiber)
    GTEST_SKIP() << "could not convert the test thread to a fiber";

  FiberContextTestState State;
  State.DispatcherFiber = DispatcherFiber;
  void *WorkerFiber = CreateFiber(0, runFiberContextTest, &State);
  if (!WorkerFiber) {
    if (ConvertedThread)
      (void)ConvertFiberToThread();
    GTEST_SKIP() << "could not create the test fiber";
  }

  EXPECT_FALSE(hasCurrentStaticArena());
  const void *DispatcherCommandLineContext = cl::getCurrentContextIdentity();
  SwitchToFiber(WorkerFiber);
  EXPECT_EQ(1u, State.Visits);
  EXPECT_FALSE(hasCurrentStaticArena());
  EXPECT_EQ(DispatcherCommandLineContext, cl::getCurrentContextIdentity());

  SwitchToFiber(WorkerFiber);
  EXPECT_EQ(2u, State.Visits);
  EXPECT_FALSE(hasCurrentStaticArena());
  EXPECT_EQ(DispatcherCommandLineContext, cl::getCurrentContextIdentity());

  // Resume once more so the worker unwinds its command-line and arena owners.
  // Deleting a fiber while those C++ scopes are suspended would leak them.
  SwitchToFiber(WorkerFiber);
  EXPECT_TRUE(State.Finished);
  EXPECT_FALSE(hasCurrentStaticArena());
  EXPECT_EQ(DispatcherCommandLineContext, cl::getCurrentContextIdentity());

  DeleteFiber(WorkerFiber);
  if (ConvertedThread)
    EXPECT_TRUE(ConvertFiberToThread());
#endif
}

TEST(StaticArenaTest, CombinedCaptureFailureRollsBackCommandLineLease) {
#if !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  {
    ScopedStaticArenaBinding ArenaBinding(*Arena);
    cl::ScopedContext CommandLineContext;
    cl::ContextToken PriorLease = cl::ContextToken::capture();

    Arena->beginClosing();
    ToolExecutionContext Result;
    EXPECT_FALSE(ToolExecutionContextTestPeer::tryCapture(Result));
    EXPECT_TRUE(Result.empty());

    // Releasing the lease that predated the failed combined capture must leave
    // exactly the owner's lease, allowing command-line teardown to start.
    PriorLease = cl::ContextToken();
    CommandLineContext.beginClosing();
    Arena->runDestructors();
  }
#endif
}

} // namespace
