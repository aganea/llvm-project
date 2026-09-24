//===- llvm/unittest/Support/ParallelTest.cpp -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Parallel.h unit tests.
///
//===----------------------------------------------------------------------===//

#include "llvm/Support/Parallel.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Config/llvm-config.h" // for LLVM_ENABLE_THREADS
#include "llvm/Support/Compiler.h"
#include "llvm/Support/StaticArena.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/ToolExecutionContext.h"
#include "gtest/gtest.h"
#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

uint32_t array[1024 * 1024];

using namespace llvm;

static bool supportsStaticArenaRecords() {
#if defined(_WIN32) || defined(__ELF__) || defined(__wasm__)
  return true;
#else
  return false;
#endif
}

// Tests below are hanging up on mingw. Investigating.
#if !defined(__MINGW32__)

TEST(Parallel, sort) {
  std::mt19937 randEngine;
  std::uniform_int_distribution<uint32_t> dist;

  for (auto &i : array)
    i = dist(randEngine);

  parallelSort(std::begin(array), std::end(array));
  ASSERT_TRUE(llvm::is_sorted(array));
}

TEST(Parallel, parallel_for) {
  // We need to test the case with a TaskSize > 1. We are white-box testing
  // here. The TaskSize is calculated as (End - Begin) / 1024 at the time of
  // writing.
  uint32_t range[2050];
  std::fill(range, range + 2050, 1);
  parallelFor(0, 2049, [&range](size_t I) { ++range[I]; });

  uint32_t expected[2049];
  std::fill(expected, expected + 2049, 2);
  ASSERT_TRUE(std::equal(range, range + 2049, expected));
  // Check that we don't write past the end of the requested range.
  ASSERT_EQ(range[2049], 1u);
}

TEST(Parallel, TransformReduce) {
  // Sum an empty list, check that it works.
  auto identity = [](uint32_t v) { return v; };
  uint32_t sum = parallelTransformReduce(ArrayRef<uint32_t>(), 0U,
                                         std::plus<uint32_t>(), identity);
  EXPECT_EQ(sum, 0U);

  // Sum the lengths of these strings in parallel.
  const char *strs[] = {"a", "ab", "abc", "abcd", "abcde", "abcdef"};
  size_t lenSum =
      parallelTransformReduce(strs, static_cast<size_t>(0), std::plus<size_t>(),
                              [](const char *s) { return strlen(s); });
  EXPECT_EQ(lenSum, static_cast<size_t>(21));

  // Check that we handle non-divisible task sizes as above.
  uint32_t range[2050];
  llvm::fill(range, 1);
  sum = parallelTransformReduce(range, 0U, std::plus<uint32_t>(), identity);
  EXPECT_EQ(sum, 2050U);

  llvm::fill(range, 2);
  sum = parallelTransformReduce(range, 0U, std::plus<uint32_t>(), identity);
  EXPECT_EQ(sum, 4100U);

  // Avoid one large task.
  uint32_t range2[3060];
  llvm::fill(range2, 1);
  sum = parallelTransformReduce(range2, 0U, std::plus<uint32_t>(), identity);
  EXPECT_EQ(sum, 3060U);
}

TEST(Parallel, ForEachError) {
  int nums[] = {1, 2, 3, 4, 5, 6};
  Error e = parallelForEachError(nums, [](int v) -> Error {
    if ((v & 1) == 0)
      return createStringError(std::errc::invalid_argument, "asdf");
    return Error::success();
  });
  EXPECT_TRUE(e.isA<ErrorList>());
  std::string errText = toString(std::move(e));
  EXPECT_EQ(errText, std::string("asdf\nasdf\nasdf"));
}

#if LLVM_ENABLE_THREADS
TEST(Parallel, LegacyExecutorRetainsFirstUseWidth) {
  ThreadPoolStrategy SavedStrategy = parallel::strategy;
  scope_exit RestoreStrategy([&] { parallel::strategy = SavedStrategy; });

  const size_t FirstUseWidth = parallel::getThreadCount();
  parallel::strategy = hardware_concurrency(1);
  EXPECT_EQ(parallel::getThreadCount(), FirstUseWidth);
}

TEST(Parallel, InvocationThreadLimitDoesNotResizeProcessPool) {
  if (!supportsStaticArenaRecords())
    GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";

  ThreadPoolStrategy SavedStrategy = parallel::strategy;
  scope_exit RestoreStrategy([&] { parallel::strategy = SavedStrategy; });
  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  ScopedStaticArenaBinding ArenaBinding(*Arena);

  // An arena invocation's strategy is a scheduling quota, not a request to
  // replace or resize the process-wide physical pool.
  parallel::strategy = hardware_concurrency(2);
  const unsigned Limit = parallel::getThreadCount();
  ASSERT_GE(Limit, 1u);
  ASSERT_LE(Limit, 2u);

  std::atomic<unsigned> Started{0};
  std::atomic<unsigned> Active{0};
  std::atomic<unsigned> MaxActive{0};
  std::atomic<bool> Release{false};
  {
    parallel::TaskGroup TG;
    for (unsigned I = 0; I != 16; ++I) {
      TG.spawn([&] {
        unsigned Current = Active.fetch_add(1, std::memory_order_relaxed) + 1;
        unsigned OldMax = MaxActive.load(std::memory_order_relaxed);
        while (Current > OldMax &&
               !MaxActive.compare_exchange_weak(OldMax, Current,
                                                std::memory_order_relaxed)) {
        }
        Started.fetch_add(1, std::memory_order_release);
        while (!Release.load(std::memory_order_acquire))
          std::this_thread::yield();
        Active.fetch_sub(1, std::memory_order_relaxed);
      });
    }

    while (Started.load(std::memory_order_acquire) < Limit)
      std::this_thread::yield();
    // Give an incorrectly unbounded executor time to start additional work.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Release.store(true, std::memory_order_release);
  }

  EXPECT_EQ(MaxActive.load(std::memory_order_relaxed), Limit);

  Arena->beginClosing();
  Arena->runDestructors();
}

TEST(Parallel, ConcurrentArenasHaveIndependentQuotas) {
  if (!supportsStaticArenaRecords())
    GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";

  if (hardware_concurrency().compute_thread_count() < 4)
    GTEST_SKIP() << "requires four physical executor workers";

  ThreadPoolStrategy SavedStrategy = parallel::strategy;
  scope_exit RestoreStrategy([&] { parallel::strategy = SavedStrategy; });

  std::unique_ptr<StaticArena> FirstArena = StaticArena::create();
  std::unique_ptr<StaticArena> SecondArena = StaticArena::create();
  for (StaticArena *Arena : {FirstArena.get(), SecondArena.get()}) {
    ScopedStaticArenaBinding ArenaBinding(*Arena);
    parallel::strategy = hardware_concurrency(2);
  }
  std::array<std::atomic<unsigned>, 2> Active{};
  std::array<std::atomic<unsigned>, 2> MaxActive{};
  std::atomic<unsigned> TotalActive{0};
  std::atomic<unsigned> MaxTotalActive{0};
  std::atomic<unsigned> Started{0};
  std::atomic<unsigned> Submitted{0};
  std::atomic<bool> Release{false};

  auto UpdateMax = [](std::atomic<unsigned> &Maximum, unsigned Value) {
    unsigned OldMaximum = Maximum.load(std::memory_order_relaxed);
    while (Value > OldMaximum &&
           !Maximum.compare_exchange_weak(OldMaximum, Value,
                                          std::memory_order_relaxed)) {
    }
  };

  auto Caller = [&](unsigned Index, StaticArena &Arena) {
    ScopedStaticArenaBinding ArenaBinding(Arena);
    {
      parallel::TaskGroup TG;
      for (unsigned I = 0; I != 8; ++I) {
        TG.spawn([&, Index] {
          unsigned ArenaActive =
              Active[Index].fetch_add(1, std::memory_order_relaxed) + 1;
          UpdateMax(MaxActive[Index], ArenaActive);
          unsigned Total =
              TotalActive.fetch_add(1, std::memory_order_relaxed) + 1;
          UpdateMax(MaxTotalActive, Total);
          Started.fetch_add(1, std::memory_order_release);
          while (!Release.load(std::memory_order_acquire))
            std::this_thread::yield();
          TotalActive.fetch_sub(1, std::memory_order_relaxed);
          Active[Index].fetch_sub(1, std::memory_order_relaxed);
        });
      }
      Submitted.fetch_add(1, std::memory_order_release);
    }
    Arena.beginClosing();
    Arena.runDestructors();
  };

  std::thread FirstCaller(Caller, 0, std::ref(*FirstArena));
  std::thread SecondCaller(Caller, 1, std::ref(*SecondArena));
  while (Submitted.load(std::memory_order_acquire) != 2)
    std::this_thread::yield();

  auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (Started.load(std::memory_order_acquire) < 4 &&
         std::chrono::steady_clock::now() < Deadline)
    std::this_thread::yield();
  unsigned StartedBeforeRelease = Started.load(std::memory_order_acquire);
  Release.store(true, std::memory_order_release);
  FirstCaller.join();
  SecondCaller.join();

  EXPECT_GE(StartedBeforeRelease, 4u);
  EXPECT_LE(MaxActive[0].load(std::memory_order_relaxed), 2u);
  EXPECT_LE(MaxActive[1].load(std::memory_order_relaxed), 2u);
  EXPECT_LE(MaxTotalActive.load(std::memory_order_relaxed), 4u);
}

TEST(Parallel, ArenasReuseOneProcessWorkerPool) {
  if (!supportsStaticArenaRecords())
    GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";

#if LLVM_THREAD_SANITIZER_BUILD
  // This identity check deliberately blocks every physical worker at once.
  // Starting a full-width pool under TSan can exceed its fixed deadline; the
  // race-sensitive concurrent-arena tests below still exercise the pool.
  GTEST_SKIP() << "full-pool identity barrier is unsuitable under TSan";
#endif

  const unsigned PhysicalWidth = hardware_concurrency().compute_thread_count();
  if (PhysicalWidth > 64)
    GTEST_SKIP() << "avoids creating an excessive number of blocked tasks";

  ThreadPoolStrategy SavedStrategy = parallel::strategy;
  scope_exit RestoreStrategy([&] { parallel::strategy = SavedStrategy; });
  std::unique_ptr<StaticArena> FirstArena = StaticArena::create();
  std::unique_ptr<StaticArena> SecondArena = StaticArena::create();

  auto CaptureWorkers = [&](StaticArena &Arena,
                            std::set<std::thread::id> &WorkerIDs) {
    ScopedStaticArenaBinding ArenaBinding(Arena);
    parallel::strategy = hardware_concurrency(PhysicalWidth);
    std::mutex WorkerIDsMutex;
    std::atomic<unsigned> Started{0};
    std::atomic<bool> Release{false};
    bool AllWorkersStarted = false;
    {
      parallel::TaskGroup TG;
      for (unsigned I = 0; I != PhysicalWidth; ++I) {
        TG.spawn([&] {
          {
            std::lock_guard<std::mutex> Lock(WorkerIDsMutex);
            WorkerIDs.insert(std::this_thread::get_id());
          }
          Started.fetch_add(1, std::memory_order_release);
          while (!Release.load(std::memory_order_acquire))
            std::this_thread::yield();
        });
      }

      auto Deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (Started.load(std::memory_order_acquire) != PhysicalWidth &&
             std::chrono::steady_clock::now() < Deadline)
        std::this_thread::yield();
      AllWorkersStarted =
          Started.load(std::memory_order_acquire) == PhysicalWidth;
      Release.store(true, std::memory_order_release);
    }
    return AllWorkersStarted && WorkerIDs.size() == PhysicalWidth;
  };

  std::set<std::thread::id> FirstWorkerIDs;
  std::set<std::thread::id> SecondWorkerIDs;
  bool CapturedFirst = CaptureWorkers(*FirstArena, FirstWorkerIDs);
  bool CapturedSecond = CaptureWorkers(*SecondArena, SecondWorkerIDs);

  // Keeping both arenas alive distinguishes one process pool from separate
  // per-arena pools: live std::threads cannot share thread IDs.
  if (CapturedFirst && CapturedSecond)
    EXPECT_EQ(FirstWorkerIDs, SecondWorkerIDs);

  for (StaticArena *Arena : {FirstArena.get(), SecondArena.get()}) {
    ScopedStaticArenaBinding ArenaBinding(*Arena);
    Arena->beginClosing();
    Arena->runDestructors();
  }

  EXPECT_TRUE(CapturedFirst);
  EXPECT_TRUE(CapturedSecond);
}

TEST(Parallel, ConcurrentCallersShareOneArenaQuota) {
  if (!supportsStaticArenaRecords())
    GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";

  ThreadPoolStrategy SavedStrategy = parallel::strategy;
  scope_exit RestoreStrategy([&] { parallel::strategy = SavedStrategy; });
  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  ScopedStaticArenaBinding ArenaBinding(*Arena);
  parallel::strategy = hardware_concurrency(2);
  const unsigned Limit = parallel::getThreadCount();

  std::atomic<unsigned> Active{0};
  std::atomic<unsigned> MaxActive{0};
  std::atomic<unsigned> Started{0};
  std::atomic<unsigned> Submitted{0};
  std::array<std::atomic<unsigned>, 2> Completed{};
  std::atomic<bool> Release{false};

  ToolExecutionContext FirstContext = ToolExecutionContext::capture();
  ToolExecutionContext SecondContext = FirstContext;
  auto Caller = [&](unsigned Index, ToolExecutionContext Context) {
    ScopedToolExecutionContext Binding(std::move(Context));
    {
      parallel::TaskGroup TG;
      for (unsigned I = 0; I != 8; ++I) {
        TG.spawn([&, Index] {
          unsigned Current = Active.fetch_add(1, std::memory_order_relaxed) + 1;
          unsigned OldMaximum = MaxActive.load(std::memory_order_relaxed);
          while (Current > OldMaximum &&
                 !MaxActive.compare_exchange_weak(OldMaximum, Current,
                                                  std::memory_order_relaxed)) {
          }
          Started.fetch_add(1, std::memory_order_release);
          while (!Release.load(std::memory_order_acquire))
            std::this_thread::yield();
          Active.fetch_sub(1, std::memory_order_relaxed);
          Completed[Index].fetch_add(1, std::memory_order_relaxed);
        });
      }
      Submitted.fetch_add(1, std::memory_order_release);
    }
  };

  std::thread First(Caller, 0, std::move(FirstContext));
  std::thread Second(Caller, 1, std::move(SecondContext));
  while (Submitted.load(std::memory_order_acquire) != 2)
    std::this_thread::yield();

  auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (Started.load(std::memory_order_acquire) < Limit &&
         std::chrono::steady_clock::now() < Deadline)
    std::this_thread::yield();
  Release.store(true, std::memory_order_release);
  First.join();
  Second.join();

  EXPECT_GE(Started.load(std::memory_order_relaxed), Limit);
  EXPECT_LE(MaxActive.load(std::memory_order_relaxed), Limit);
  EXPECT_EQ(Completed[0].load(std::memory_order_relaxed), 8u);
  EXPECT_EQ(Completed[1].load(std::memory_order_relaxed), 8u);

  Arena->beginClosing();
  Arena->runDestructors();
}

TEST(Parallel, SingleThreadArenaQuotaDoesNotRunInlineOnCallers) {
  if (!supportsStaticArenaRecords())
    GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";

  ThreadPoolStrategy SavedStrategy = parallel::strategy;
  scope_exit RestoreStrategy([&] { parallel::strategy = SavedStrategy; });
  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  ScopedStaticArenaBinding ArenaBinding(*Arena);
  parallel::strategy = hardware_concurrency(1);

  std::atomic<unsigned> Ready{0};
  std::atomic<bool> Start{false};
  std::atomic<unsigned> Active{0};
  std::atomic<unsigned> MaxActive{0};
  ToolExecutionContext FirstContext = ToolExecutionContext::capture();
  ToolExecutionContext SecondContext = FirstContext;

  auto Caller = [&](ToolExecutionContext Context) {
    ScopedToolExecutionContext Binding(std::move(Context));
    Ready.fetch_add(1, std::memory_order_release);
    while (!Start.load(std::memory_order_acquire))
      std::this_thread::yield();

    parallel::TaskGroup TG;
    TG.spawn([&] {
      unsigned Current = Active.fetch_add(1, std::memory_order_relaxed) + 1;
      unsigned OldMaximum = MaxActive.load(std::memory_order_relaxed);
      while (Current > OldMaximum &&
             !MaxActive.compare_exchange_weak(OldMaximum, Current,
                                              std::memory_order_relaxed)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      Active.fetch_sub(1, std::memory_order_relaxed);
    });
  };

  std::thread First(Caller, std::move(FirstContext));
  std::thread Second(Caller, std::move(SecondContext));
  while (Ready.load(std::memory_order_acquire) != 2)
    std::this_thread::yield();
  Start.store(true, std::memory_order_release);
  First.join();
  Second.join();

  EXPECT_EQ(MaxActive.load(std::memory_order_relaxed), 1u);

  Arena->beginClosing();
  Arena->runDestructors();
}

TEST(Parallel, SingleThreadArenaPreservesSubmissionOrder) {
  if (!supportsStaticArenaRecords())
    GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";

  ThreadPoolStrategy SavedStrategy = parallel::strategy;
  scope_exit RestoreStrategy([&] { parallel::strategy = SavedStrategy; });
  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  ScopedStaticArenaBinding ArenaBinding(*Arena);
  parallel::strategy = hardware_concurrency(1);

  std::atomic<bool> BlockerStarted{false};
  std::atomic<bool> ReleaseBlocker{false};
  std::vector<unsigned> Order;
  {
    parallel::TaskGroup TG;
    TG.spawn([&] {
      BlockerStarted.store(true, std::memory_order_release);
      while (!ReleaseBlocker.load(std::memory_order_acquire))
        std::this_thread::yield();
      Order.push_back(0);
    });

    while (!BlockerStarted.load(std::memory_order_acquire))
      std::this_thread::yield();
    for (unsigned I = 1; I != 8; ++I)
      TG.spawn([&, I] { Order.push_back(I); });
    ReleaseBlocker.store(true, std::memory_order_release);
  }

  EXPECT_EQ(Order, (std::vector<unsigned>{0, 1, 2, 3, 4, 5, 6, 7}));

  Arena->beginClosing();
  Arena->runDestructors();
}

TEST(Parallel, SingleThreadArenaWorkerRunsNestedAlgorithmsInline) {
  if (!supportsStaticArenaRecords())
    GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";

  ThreadPoolStrategy SavedStrategy = parallel::strategy;
  scope_exit RestoreStrategy([&] { parallel::strategy = SavedStrategy; });
  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  ScopedStaticArenaBinding ArenaBinding(*Arena);
  parallel::strategy = hardware_concurrency(1);

  const std::thread::id Caller = std::this_thread::get_id();
  std::atomic<bool> TopLevelRanOnCaller{false};
  std::vector<unsigned> TopLevelValues{1, 2, 3, 4};
  unsigned TopLevelSum = parallelTransformReduce(
      TopLevelValues, 0u, std::plus<unsigned>(), [&](unsigned Value) {
        if (std::this_thread::get_id() == Caller)
          TopLevelRanOnCaller.store(true, std::memory_order_relaxed);
        return Value;
      });

  std::atomic<bool> OuterRanOnCaller{false};
  std::atomic<bool> NestedChangedWorker{false};
  unsigned Sum = 0;
  {
    parallel::TaskGroup Outer;
    Outer.spawn([&] {
      const std::thread::id Worker = std::this_thread::get_id();
      OuterRanOnCaller.store(Worker == Caller, std::memory_order_relaxed);
      auto CheckWorker = [&] {
        if (std::this_thread::get_id() != Worker)
          NestedChangedWorker.store(true, std::memory_order_relaxed);
      };

      parallelFor(0, 32, [&](size_t) { CheckWorker(); });
      std::vector<unsigned> Values{4, 2, 3, 1};
      parallelSort(Values, [&](unsigned L, unsigned R) {
        CheckWorker();
        return L < R;
      });
      Sum = parallelTransformReduce(
          Values, 0u, std::plus<unsigned>(), [&](unsigned Value) {
            CheckWorker();
            return Value;
          });
    });
  }

  EXPECT_EQ(TopLevelSum, 10u);
  EXPECT_FALSE(TopLevelRanOnCaller.load(std::memory_order_relaxed));
  EXPECT_FALSE(OuterRanOnCaller.load(std::memory_order_relaxed));
  EXPECT_FALSE(NestedChangedWorker.load(std::memory_order_relaxed));
  EXPECT_EQ(Sum, 10u);

  Arena->beginClosing();
  Arena->runDestructors();
}

TEST(Parallel, CallableCapturesAreDestroyedInInvocationContext) {
  if (!supportsStaticArenaRecords())
    GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";

  struct Capture {
    std::atomic<bool> &Destroyed;
    std::atomic<bool> &DestroyedInArena;

    Capture(std::atomic<bool> &Destroyed, std::atomic<bool> &DestroyedInArena)
        : Destroyed(Destroyed), DestroyedInArena(DestroyedInArena) {}

    ~Capture() {
      DestroyedInArena.store(hasCurrentStaticArena(),
                             std::memory_order_relaxed);
      Destroyed.store(true, std::memory_order_release);
    }
  };

  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  ScopedStaticArenaBinding ArenaBinding(*Arena);
  std::atomic<bool> Destroyed{false};
  std::atomic<bool> DestroyedInArena{false};
  std::atomic<bool> Run{false};

  {
    parallel::TaskGroup TG;
    auto State = std::make_shared<Capture>(Destroyed, DestroyedInArena);
    TG.spawn([State, &Run] {
      while (!Run.load(std::memory_order_acquire))
        std::this_thread::yield();
    });
    State.reset();
    Run.store(true, std::memory_order_release);
  }

  EXPECT_TRUE(Destroyed.load(std::memory_order_acquire));
  EXPECT_TRUE(DestroyedInArena.load(std::memory_order_relaxed));

  Arena->beginClosing();
  Arena->runDestructors();
}

TEST(Parallel, ArenaAlgorithmsDoNotBypassSharedPoolOnCaller) {
  if (!supportsStaticArenaRecords())
    GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";

  ThreadPoolStrategy SavedStrategy = parallel::strategy;
  scope_exit RestoreStrategy([&] { parallel::strategy = SavedStrategy; });
  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  ScopedStaticArenaBinding ArenaBinding(*Arena);
  parallel::strategy = hardware_concurrency(2);

  const std::thread::id Caller = std::this_thread::get_id();
  std::atomic<bool> RanOnCaller{false};
  parallelFor(0, 64, [&](size_t) {
    if (std::this_thread::get_id() == Caller)
      RanOnCaller.store(true, std::memory_order_relaxed);
  });
  EXPECT_FALSE(RanOnCaller.load(std::memory_order_relaxed));

  std::vector<unsigned> Values(2048);
  for (unsigned I = 0; I != Values.size(); ++I)
    Values[I] = Values.size() - I;
  parallelSort(Values.begin(), Values.end(), [&](unsigned L, unsigned R) {
    if (std::this_thread::get_id() == Caller)
      RanOnCaller.store(true, std::memory_order_relaxed);
    return L < R;
  });
  EXPECT_TRUE(llvm::is_sorted(Values));
  EXPECT_FALSE(RanOnCaller.load(std::memory_order_relaxed));

  Arena->beginClosing();
  Arena->runDestructors();
}

TEST(Parallel, ArenaNestedParallelForUsesQuotaReentrantly) {
  if (!supportsStaticArenaRecords())
    GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";

  if (hardware_concurrency().compute_thread_count() < 2)
    GTEST_SKIP() << "requires two physical executor workers";

  ThreadPoolStrategy SavedStrategy = parallel::strategy;
  scope_exit RestoreStrategy([&] { parallel::strategy = SavedStrategy; });
  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  ScopedStaticArenaBinding ArenaBinding(*Arena);
  parallel::strategy = hardware_concurrency(2);

  std::atomic<unsigned> OuterStarted{0};
  std::atomic<unsigned> InnerCompleted{0};
  parallelFor(0, 2, [&](size_t) {
    OuterStarted.fetch_add(1, std::memory_order_release);
    while (OuterStarted.load(std::memory_order_acquire) != 2)
      std::this_thread::yield();
    parallelFor(0, 8, [&](size_t) {
      InnerCompleted.fetch_add(1, std::memory_order_relaxed);
    });
  });
  EXPECT_EQ(InnerCompleted.load(std::memory_order_relaxed), 16u);

  Arena->beginClosing();
  Arena->runDestructors();
}

TEST(Parallel, NestedInvocationCanHelpEnclosingContextTask) {
  if (!supportsStaticArenaRecords())
    GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";

  if (hardware_concurrency().compute_thread_count() < 3)
    GTEST_SKIP() << "requires three physical executor workers";

  ThreadPoolStrategy SavedStrategy = parallel::strategy;
  scope_exit RestoreStrategy([&] { parallel::strategy = SavedStrategy; });
  std::unique_ptr<StaticArena> ArenaA = StaticArena::create();
  std::unique_ptr<StaticArena> ArenaB = StaticArena::create();
  {
    ScopedStaticArenaBinding ArenaABinding(*ArenaA);
    parallel::strategy = hardware_concurrency(2);

    std::atomic<unsigned> OuterStarted{0};
    std::atomic<bool> NestedStarted{false};
    std::atomic<bool> EnclosingTaskRan{false};
    std::atomic<bool> ReleaseBlocker{false};
    std::atomic<bool> BlockerTimedOut{false};

    {
      parallel::TaskGroup Outer;
      Outer.spawn([&] {
        ToolExecutionContext ArenaAContext = ToolExecutionContext::capture();
        OuterStarted.fetch_add(1, std::memory_order_release);
        while (OuterStarted.load(std::memory_order_acquire) != 2)
          std::this_thread::yield();

        ScopedStaticArenaBinding ArenaBBinding(*ArenaB);
        parallel::TaskGroup NestedInvocation;
        NestedInvocation.spawn([&, ArenaAContext =
                                       std::move(ArenaAContext)]() mutable {
          NestedStarted.store(true, std::memory_order_release);
          parallel::TaskGroup MixedContext;
          {
            ScopedToolExecutionContext RestoreArenaA(std::move(ArenaAContext));
            MixedContext.spawn([&] {
              EnclosingTaskRan.store(true, std::memory_order_release);
              ReleaseBlocker.store(true, std::memory_order_release);
            });
          }
          // MixedContext is destroyed while B is current, but its queued
          // work belongs to the enclosing A invocation. The A quota is
          // already saturated by the two Outer tasks, so only reentrant
          // latch-targeted helping can make progress.
        });

        // Do not reach NestedInvocation's helping destructor until an idle
        // physical worker has stolen the B task. Waiting must lend this A
        // worker's quota permit so the mixed-context dependency can run; a
        // copied ancestry exemption would make progress but could exceed A's
        // configured concurrency under fan-out.
        while (!NestedStarted.load(std::memory_order_acquire))
          std::this_thread::yield();
      });

      Outer.spawn([&] {
        OuterStarted.fetch_add(1, std::memory_order_release);
        auto Deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ReleaseBlocker.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < Deadline)
          std::this_thread::yield();
        bool Expected = false;
        if (ReleaseBlocker.compare_exchange_strong(Expected, true,
                                                   std::memory_order_acq_rel))
          BlockerTimedOut.store(true, std::memory_order_relaxed);
      });
    }

    EXPECT_TRUE(EnclosingTaskRan.load(std::memory_order_acquire));
    EXPECT_FALSE(BlockerTimedOut.load(std::memory_order_relaxed));

    ArenaA->beginClosing();
    ArenaA->runDestructors();
  }
  {
    ScopedStaticArenaBinding ArenaBBinding(*ArenaB);
    ArenaB->beginClosing();
    ArenaB->runDestructors();
  }
}

TEST(Parallel, NestedInvocationFanoutDoesNotBypassEnclosingQuota) {
  if (!supportsStaticArenaRecords())
    GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";

  if (hardware_concurrency().compute_thread_count() < 4)
    GTEST_SKIP() << "requires four physical executor workers";

  ThreadPoolStrategy SavedStrategy = parallel::strategy;
  scope_exit RestoreStrategy([&] { parallel::strategy = SavedStrategy; });
  std::unique_ptr<StaticArena> ArenaA = StaticArena::create();
  std::unique_ptr<StaticArena> ArenaB = StaticArena::create();

  {
    ScopedStaticArenaBinding ArenaBBinding(*ArenaB);
    parallel::strategy = hardware_concurrency(4);
  }
  {
    ScopedStaticArenaBinding ArenaABinding(*ArenaA);
    parallel::strategy = hardware_concurrency(1);

    std::atomic<unsigned> ActiveA{0};
    std::atomic<unsigned> MaxActiveA{0};
    std::atomic<unsigned> CompletedA{0};
    {
      parallel::TaskGroup Outer;
      Outer.spawn([&] {
        ToolExecutionContext ArenaAContext = ToolExecutionContext::capture();
        ScopedStaticArenaBinding ArenaBBinding(*ArenaB);
        parallel::TaskGroup Fanout;
        for (unsigned I = 0; I != 8; ++I) {
          Fanout.spawn([&, ArenaAContext]() mutable {
            parallel::TaskGroup BackIntoA;
            {
              ScopedToolExecutionContext RestoreArenaA(
                  std::move(ArenaAContext));
              BackIntoA.spawn([&] {
                unsigned Current =
                    ActiveA.fetch_add(1, std::memory_order_relaxed) + 1;
                unsigned OldMaximum =
                    MaxActiveA.load(std::memory_order_relaxed);
                while (Current > OldMaximum &&
                       !MaxActiveA.compare_exchange_weak(
                           OldMaximum, Current, std::memory_order_relaxed)) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ActiveA.fetch_sub(1, std::memory_order_relaxed);
                CompletedA.fetch_add(1, std::memory_order_relaxed);
              });
            }
          });
        }
      });
    }

    EXPECT_EQ(CompletedA.load(std::memory_order_relaxed), 8u);
    EXPECT_EQ(MaxActiveA.load(std::memory_order_relaxed), 1u);

    ArenaA->beginClosing();
    ArenaA->runDestructors();
  }
  {
    ScopedStaticArenaBinding ArenaBBinding(*ArenaB);
    ArenaB->beginClosing();
    ArenaB->runDestructors();
  }
}

TEST(Parallel, NestedTaskGroup) {
  parallel::TaskGroup tg;
  EXPECT_TRUE(tg.isParallel() || (parallel::strategy.ThreadsRequested == 1));

  tg.spawn([&]() {
    parallel::TaskGroup nestedTG;
    EXPECT_TRUE(nestedTG.isParallel() ||
                (parallel::strategy.ThreadsRequested == 1));
  });
}

// Verify nested parallelFor doesn't deadlock. This is a simplified version of
// the pattern from https://reviews.llvm.org/D61115 that originally motivated
// serializing nested TaskGroups. With work-stealing in helpSync(), nested
// parallelism now works without deadlock.
TEST(Parallel, NestedParallelFor) {
  std::atomic<uint32_t> count{0};
  parallelFor(0, 8, [&](size_t i) {
    parallelFor(0, 8, [&](size_t j) {
      parallelFor(0, 8, [&](size_t k) {
        count.fetch_add(1, std::memory_order_relaxed);
      });
    });
  });
  EXPECT_EQ(count.load(), 512u);
}
#endif

#endif
