//===- llvm/Support/Parallel.cpp - Parallel algorithms --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Parallel.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/ExponentialBackoff.h"
#include "llvm/Support/Jobserver.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/StaticArena.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/ToolExecutionContext.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

using namespace llvm;
using namespace llvm::parallel;

llvm::ThreadPoolStrategy parallel::strategy;

#if LLVM_ENABLE_THREADS

namespace {

class ThreadPoolExecutor;

struct ActiveExecution {
  ThreadPoolExecutor *Executor;
  const void *Invocation;
  ToolExecutionContext Context;
  unsigned MaxConcurrency;
  bool Counted;
  JobSlot *Slot;
  ActiveExecution *Previous;
};

// Keep this as a pointer into ordinary worker stacks instead of a TLS
// container. Process executors intentionally survive LLVM shutdown on some
// hosts, so their workers must not depend on a non-trivial TLS destructor.
static thread_local ActiveExecution *CurrentExecution = nullptr;

/// Runs closures on a thread pool. Wider invocations use filo order, while a
/// width-one invocation preserves the serial submission order it replaces.
class ThreadPoolExecutor {
public:
  explicit ThreadPoolExecutor(ThreadPoolStrategy S, bool UseInvocationQuotas)
      : UseInvocationQuotas(UseInvocationQuotas) {
    if (S.UseJobserver)
      TheJobserver = JobserverClient::getInstance();

    ThreadCount = S.compute_thread_count();
    // Spawn all but one of the threads in another thread as spawning threads
    // can take a while.
    Threads.reserve(ThreadCount);
    Threads.resize(1);
    std::lock_guard<std::mutex> Lock(Mutex);
    // Use operator[] before creating the thread to avoid data race in .size()
    // in 'safe libc++' mode.
    auto &Thread0 = Threads[0];
    Thread0 = std::thread([this, S] {
      for (unsigned I = 1; I < ThreadCount; ++I) {
        Threads.emplace_back([this, S, I] { work(S, I); });
        if (Stop)
          break;
      }
      ThreadsCreated.set_value();
      work(S, 0);
    });
  }

  // To make sure the thread pool executor can only be created with a parallel
  // strategy.
  ThreadPoolExecutor() = delete;

  void stop() {
    {
      std::lock_guard<std::mutex> Lock(Mutex);
      if (Stop)
        return;
      Stop = true;
    }
    Cond.notify_all();
    ThreadsCreated.get_future().wait();

    std::thread::id CurrentThreadId = std::this_thread::get_id();
    for (std::thread &T : Threads)
      if (T.get_id() == CurrentThreadId)
        T.detach();
      else
        T.join();
  }

  ~ThreadPoolExecutor() { stop(); }

  struct LegacyCreator {
    static void *call() { return new ThreadPoolExecutor(strategy, false); }
  };
  struct HardwareCreator {
    static void *call() {
      return new ThreadPoolExecutor(hardware_concurrency(), true);
    }
  };
  struct JobserverCreator {
    static void *call() {
      return new ThreadPoolExecutor(jobserver_concurrency(), true);
    }
  };
  struct Deleter {
    static void call(void *Ptr) { ((ThreadPoolExecutor *)Ptr)->stop(); }
  };

  struct WorkItem {
    std::function<void()> F;
    std::reference_wrapper<parallel::detail::Latch> L;
    ToolExecutionContext Context;
    const void *Invocation;
    unsigned MaxConcurrency;
  };

  void add(std::function<void()> F, parallel::detail::Latch &L) {
    ToolExecutionContext Context = ToolExecutionContext::capture();
    const void *Invocation =
        UseInvocationQuotas ? getCurrentStaticArenaIdentity() : nullptr;
    assert((!UseInvocationQuotas || Invocation) &&
           "shared executor work requires an arena identity");
    unsigned MaxConcurrency = ThreadCount;
    if (UseInvocationQuotas) {
      MaxConcurrency = std::min(ThreadCount, strategy.compute_thread_count());
      MaxConcurrency = std::max(1u, MaxConcurrency);
    }

    {
      std::lock_guard<std::mutex> Lock(Mutex);
      WorkStack.push_back({std::move(F), std::ref(L), std::move(Context),
                           Invocation, MaxConcurrency});
    }
    Cond.notify_one();
  }

  // Suspend the current task's invocation permit and execute tasks from this
  // physical pool until the latch reaches zero. A waiting task is not active
  // work, so lending its one permit preserves the invocation limit while
  // allowing a stolen nested task to schedule a dependency back into the
  // enclosing invocation. Reclaim the permit before returning to user code.
  void helpSync(const parallel::detail::Latch &L) {
    ActiveExecution *WaitingExecution = CurrentExecution;
    assert(WaitingExecution && WaitingExecution->Executor == this &&
           "can only help the current physical executor");
    assert((!TheJobserver ||
            (WaitingExecution->Slot && WaitingExecution->Slot->isValid())) &&
           "jobserver work must own a slot while it is executing");
    suspend(*WaitingExecution);

    while (L.getCount() != 0) {
      std::unique_lock<std::mutex> Lock(Mutex);
      if (Stop)
        break;

      // A helper may have lent its jobserver slot while it had no local work.
      // Reacquire one before directly executing another jobserver work item.
      if (TheJobserver && !WaitingExecution->Slot->isValid()) {
        Lock.unlock();
        if (acquireJobSlot(*WaitingExecution->Slot, &L,
                           /*RequireRunnable=*/true))
          continue;
        Lock.lock();
        if (Stop || L.getCount() == 0)
          continue;
        Cond.wait_for(Lock, std::chrono::milliseconds(1), [&] {
          return Stop || L.getCount() == 0 || hasRunnable();
        });
        continue;
      }

      // Pump all runnable work from this pool, not just work on L. This is
      // required when a normal-pool task waits for jobserver work which in
      // turn submits a dependency back to the normal pool (and vice versa).
      if (popAndRun(Lock, WaitingExecution->Slot))
        continue;

      // Waiting is not active jobserver work. Return the external token before
      // polling a cross-executor latch so another LLVM worker or make child can
      // use that capacity. It will be reacquired before any work resumes.
      Lock.unlock();
      if (TheJobserver)
        releaseJobSlot(*WaitingExecution->Slot);
      Lock.lock();

      // L may belong to the other process executor, whose completion does not
      // signal this condition variable. Poll it at a low frequency while still
      // waking immediately for newly runnable local work.
      Cond.wait_for(Lock, std::chrono::milliseconds(1), [&] {
        return Stop || L.getCount() == 0 || hasRunnable();
      });
    }

    if (!TheJobserver) {
      resume(*WaitingExecution);
      return;
    }

    // Reclaim the invocation quota and external token as one non-blocking
    // retry. Neither resource may be held while waiting for the other: doing
    // so can starve same-invocation work or another make child indefinitely.
    releaseJobSlot(*WaitingExecution->Slot);
    while (!Stop) {
      JobSlot Candidate = TheJobserver->tryAcquire();
      if (Candidate.isValid()) {
        if (tryResumeWithJobSlot(*WaitingExecution, Candidate))
          return;
        releaseJobSlot(Candidate);
      }

      // Token arrivals from another process cannot notify Cond, so retain a
      // short polling fallback in addition to local quota/token wakeups.
      std::unique_lock<std::mutex> Lock(Mutex);
      Cond.wait_for(Lock, std::chrono::milliseconds(1));
    }
  }

  size_t getThreadCount() const { return ThreadCount; }

private:
  bool isRunnable(const WorkItem &Item) const {
    if (!UseInvocationQuotas)
      return true;
    auto It = ActiveWorkers.find(Item.Invocation);
    return It == ActiveWorkers.end() || It->second < Item.MaxConcurrency;
  }

  bool hasRunnable(const void *Invocation = nullptr) const {
    if (!UseInvocationQuotas)
      return !WorkStack.empty();
    return llvm::any_of(WorkStack, [&](const WorkItem &Item) {
      return (!Invocation || Item.Invocation == Invocation) && isRunnable(Item);
    });
  }

  // Pop one runnable task from the queue and run it. Must be called with Lock
  // held; releases Lock before executing the task. Slot is null for the normal
  // executor and points at the worker's currently held token for jobserver
  // work.
  bool popAndRun(std::unique_lock<std::mutex> &Lock, JobSlot *Slot) {
    if (!UseInvocationQuotas) {
      if (WorkStack.empty())
        return false;
      auto It = WorkStack.rbegin();
      WorkItem Item = std::move(*It);
      WorkStack.erase(std::next(It).base());
      Lock.unlock();
      run(std::move(Item), Slot);
      return true;
    }

    auto ReverseIt =
        llvm::find_if(reverse(WorkStack),
                      [&](const WorkItem &Item) { return isRunnable(Item); });
    if (ReverseIt == WorkStack.rend())
      return false;

    auto It = std::next(ReverseIt).base();
    if (It->MaxConcurrency == 1) {
      // A width-one strategy historically ran TaskGroup work inline in
      // submission order. It still has to enter the shared executor so two
      // callers cannot bypass the invocation quota, but a filo queue would
      // otherwise reverse pending work and observable diagnostics. Select the
      // newest runnable invocation as usual, then its oldest pending item.
      const void *Invocation = It->Invocation;
      It = llvm::find_if(WorkStack, [&](const WorkItem &Item) {
        return Item.Invocation == Invocation;
      });
    }

    ++ActiveWorkers[It->Invocation];

    WorkItem Item = std::move(*It);
    WorkStack.erase(It);
    Lock.unlock();
    run(std::move(Item), Slot);
    return true;
  }

  bool acquireJobSlot(JobSlot &Slot,
                      const parallel::detail::Latch *WaitingFor,
                      bool RequireRunnable) {
    assert(TheJobserver && !Slot.isValid() &&
           "jobserver acquisition requires an empty slot");
    ExponentialBackoff Backoff(std::chrono::hours(24));
    do {
      if (Stop || (WaitingFor && WaitingFor->getCount() == 0))
        return false;
      if (RequireRunnable) {
        std::lock_guard<std::mutex> Lock(Mutex);
        if (!hasRunnable())
          return false;
      }

      Slot = TheJobserver->tryAcquire();
      if (Slot.isValid())
        return true;
    } while (Backoff.waitForNextAttempt());
    return false;
  }

  void releaseJobSlot(JobSlot &Slot) {
    if (!TheJobserver || !Slot.isValid())
      return;
    TheJobserver->release(std::move(Slot));
    Cond.notify_all();
  }

  void suspend(ActiveExecution &Execution) {
    if (!Execution.Counted)
      return;
    std::lock_guard<std::mutex> Lock(Mutex);
    auto It = ActiveWorkers.find(Execution.Invocation);
    assert(It != ActiveWorkers.end() && It->second != 0 &&
           "parallel executor active-worker count underflow");
    if (--It->second == 0)
      ActiveWorkers.erase(It);
    Execution.Counted = false;
    Cond.notify_all();
  }

  void resume(ActiveExecution &Execution) {
    if (!UseInvocationQuotas || Execution.Counted)
      return;
    std::unique_lock<std::mutex> Lock(Mutex);
    Cond.wait(Lock, [&] {
      auto It = ActiveWorkers.find(Execution.Invocation);
      return Stop || It == ActiveWorkers.end() ||
             It->second < Execution.MaxConcurrency;
    });
    if (!Stop) {
      ++ActiveWorkers[Execution.Invocation];
      Execution.Counted = true;
    }
  }

  bool tryResumeWithJobSlot(ActiveExecution &Execution, JobSlot &Candidate) {
    assert(TheJobserver && Candidate.isValid() && Execution.Slot &&
           !Execution.Slot->isValid() &&
           "jobserver retry requires exactly one candidate slot");

    if (!UseInvocationQuotas) {
      if (Stop)
        return false;
      *Execution.Slot = std::move(Candidate);
      return true;
    }

    // Do not keep Candidate while waiting even for this executor's mutex.
    std::unique_lock<std::mutex> Lock(Mutex, std::try_to_lock);
    if (!Lock || Stop)
      return false;
    auto It = ActiveWorkers.find(Execution.Invocation);
    if (It != ActiveWorkers.end() &&
        It->second >= Execution.MaxConcurrency)
      return false;

    ++ActiveWorkers[Execution.Invocation];
    Execution.Counted = true;
    *Execution.Slot = std::move(Candidate);
    return true;
  }

  void run(WorkItem Item, JobSlot *Slot) {
    ToolExecutionContext LocalContext =
        std::exchange(Item.Context, ToolExecutionContext());
    ActiveExecution Execution{this,
                              Item.Invocation,
                              LocalContext,
                              Item.MaxConcurrency,
                              UseInvocationQuotas,
                              Slot,
                              CurrentExecution};
    CurrentExecution = &Execution;

    {
      ScopedToolExecutionContext Binding(std::move(LocalContext));
      Item.F();
      // A callable may own invocation-local state whose destructor reaches
      // arena-backed globals. Destroy its captures before returning the task
      // lease or allowing the TaskGroup owner to tear the invocation down.
      Item.F = nullptr;

      if (Execution.Counted) {
        std::lock_guard<std::mutex> Lock(Mutex);
        auto It = ActiveWorkers.find(Item.Invocation);
        assert(It != ActiveWorkers.end() && It->second != 0 &&
               "parallel executor active-worker count underflow");
        if (--It->second == 0)
          ActiveWorkers.erase(It);
      }
    }

    assert(CurrentExecution == &Execution &&
           "parallel executor nesting stack changed out of order");
    CurrentExecution = Execution.Previous;
    // Return every invocation lease before waking the TaskGroup owner. It may
    // begin arena and command-line teardown as soon as the latch reaches zero.
    Execution.Context = ToolExecutionContext();
    Cond.notify_all();
    Item.L.get().dec();
  }

  void work(ThreadPoolStrategy S, unsigned ThreadID) {
    S.apply_thread_strategy(ThreadID);
    // Note on jobserver deadlock avoidance:
    // GNU Make grants each invoked process one implicit job slot. Our
    // JobserverClient models this by returning an implicit JobSlot on the
    // first successful tryAcquire() in a process. This guarantees forward
    // progress without requiring a dedicated "always-on" thread here.

    while (true) {
      if (TheJobserver) {
        // Wait for schedulable work before acquiring a GNU Make token. A
        // process pool can outlive many invocations, and idle or quota-blocked
        // workers must not hoard tokens from other build jobs.
        {
          std::unique_lock<std::mutex> Lock(Mutex);
          Cond.wait(Lock, [&] { return Stop || hasRunnable(); });
          if (Stop)
            return;
        }

        JobSlot Slot;
        if (!acquireJobSlot(Slot, nullptr, /*RequireRunnable=*/true))
          continue;

        {
          llvm::scope_exit SlotReleaser([&] { releaseJobSlot(Slot); });
          std::unique_lock<std::mutex> Lock(Mutex);
          if (Stop)
            return;
          popAndRun(Lock, &Slot);
        }
      } else {
        std::unique_lock<std::mutex> Lock(Mutex);
        Cond.wait(Lock, [&] { return Stop || hasRunnable(); });
        if (Stop)
          break;
        popAndRun(Lock, nullptr);
      }
    }
  }

  std::atomic<bool> Stop{false};
  std::vector<WorkItem> WorkStack;
  std::mutex Mutex;
  std::condition_variable Cond;
  std::promise<void> ThreadsCreated;
  std::vector<std::thread> Threads;
  DenseMap<const void *, unsigned> ActiveWorkers;
  unsigned ThreadCount;
  bool UseInvocationQuotas;

  JobserverClient *TheJobserver = nullptr;
};
} // namespace

static ThreadPoolExecutor *getLegacyExecutor() {
#ifdef _WIN32
  // Preserve the historical standalone lifetime. Stopping the executor from
  // llvm_shutdown() avoids process-exit races on Windows.
  static ManagedStatic<ThreadPoolExecutor, ThreadPoolExecutor::LegacyCreator,
                       ThreadPoolExecutor::Deleter>
      ManagedExec;
  static std::unique_ptr<ThreadPoolExecutor> Exec(&(*ManagedExec));
  return Exec.get();
#else
  // Standalone tools retain the historical first-use strategy and pool width.
  static ThreadPoolExecutor Exec(strategy, false);
  return &Exec;
#endif
}

static ThreadPoolExecutor *getHardwareExecutor() {
#ifdef _WIN32
  // Stopping the executor from llvm_shutdown() avoids process-exit races on
  // Windows.
  static ManagedStatic<ThreadPoolExecutor, ThreadPoolExecutor::HardwareCreator,
                       ThreadPoolExecutor::Deleter>
      ManagedExec;
  static std::unique_ptr<ThreadPoolExecutor> Exec(&(*ManagedExec));
  return Exec.get();
#else
  // The process-lifetime executor intentionally survives llvm_shutdown(),
  // avoiding TLS teardown races with worker threads.
  static ThreadPoolExecutor Exec(hardware_concurrency(), true);
  return &Exec;
#endif
}

static ThreadPoolExecutor *getJobserverExecutor() {
#ifdef _WIN32
  static ManagedStatic<ThreadPoolExecutor, ThreadPoolExecutor::JobserverCreator,
                       ThreadPoolExecutor::Deleter>
      ManagedExec;
  static std::unique_ptr<ThreadPoolExecutor> Exec(&(*ManagedExec));
  return Exec.get();
#else
  static ThreadPoolExecutor Exec(jobserver_concurrency(), true);
  return &Exec;
#endif
}

static ThreadPoolExecutor *getDefaultExecutor() {
  if (!hasCurrentStaticArena())
    return getLegacyExecutor();

  // Physical workers are process resources. Invocation-local strategies only
  // provide scheduling quotas; they never create an invocation-sized pool.
  // Keep jobserver workers separate so a normal task cannot consume a GNU Make
  // token and a jobserver task can never run without one. If both policies are
  // used concurrently the process may own both pools (up to twice the hardware
  // width when no jobserver is available); this is the deliberate exception
  // needed to preserve the opt-in jobserver contract.
  return strategy.UseJobserver ? getJobserverExecutor() : getHardwareExecutor();
}

size_t parallel::getThreadCount() {
  if (!hasCurrentStaticArena())
    return getLegacyExecutor()->getThreadCount();

  return std::max<size_t>(
      1, std::min<size_t>(getDefaultExecutor()->getThreadCount(),
                          strategy.compute_thread_count()));
}

bool parallel::detail::shouldRunCallerInExecutor() {
  bool HasArena = hasCurrentStaticArena();
  if (!strategy.UseJobserver && !HasArena)
    return false;

  ThreadPoolExecutor *Target = getDefaultExecutor();
  if (!CurrentExecution || CurrentExecution->Executor != Target)
    return true;

  // A same-executor worker may run its caller share inline only when it
  // already owns every resource required by the current context. Switching
  // arenas on one physical worker requires a permit for the new invocation;
  // switching into jobserver mode requires a token.
  if (HasArena &&
      CurrentExecution->Invocation != getCurrentStaticArenaIdentity())
    return true;
  if (strategy.UseJobserver &&
      (!CurrentExecution->Slot || !CurrentExecution->Slot->isValid()))
    return true;
  return false;
}
#endif

TaskGroup::TaskGroup()
    : Parallel(
#if LLVM_ENABLE_THREADS
          strategy.ThreadsRequested != 1 ||
              parallel::detail::shouldRunCallerInExecutor()
#else
          false
#endif
      ) {
}

TaskGroup::~TaskGroup() {
#if LLVM_ENABLE_THREADS
  // A nested TaskGroup helps its current physical executor while it waits. A
  // hardware worker must not directly run a jobserver task without a job slot,
  // but pumping all work in its own pool permits dependencies to cross between
  // the two process executors without saturating either invocation quota.
  if (Parallel && CurrentExecution)
    CurrentExecution->Executor->helpSync(L);
#endif
  L.sync();
}

void TaskGroup::spawn(std::function<void()> F) {
#if LLVM_ENABLE_THREADS
  if (Parallel) {
    L.inc();
    getDefaultExecutor()->add(std::move(F), L);
    return;
  }
#endif
  F();
}

void llvm::parallelFor(size_t Begin, size_t End,
                       function_ref<void(size_t)> Fn) {
#if LLVM_ENABLE_THREADS
  if (strategy.ThreadsRequested != 1 ||
      parallel::detail::shouldRunCallerInExecutor()) {
    size_t NumItems = End - Begin;
    if (NumItems == 0)
      return;
    // Distribute work via an atomic counter shared by NumWorkers threads,
    // keeping the task count (and thus Linux futex calls) at O(ThreadCount)
    // For lld, per-file work is somewhat uneven, so a multipler > 1 is safer.
    // While 2 vs 4 vs 8 makes no measurable difference, 4 is used as a
    // reasonable default.
    size_t NumWorkers = std::min<size_t>(NumItems, getThreadCount());
    size_t ChunkSize = std::max(size_t(1), NumItems / (NumWorkers * 4));
    std::atomic<size_t> Idx{Begin};
    auto Worker = [&] {
      while (true) {
        size_t I = Idx.fetch_add(ChunkSize, std::memory_order_relaxed);
        if (I >= End)
          break;
        size_t IEnd = std::min(I + ChunkSize, End);
        for (; I < IEnd; ++I)
          Fn(I);
      }
    };

    TaskGroup TG;
    if (parallel::detail::shouldRunCallerInExecutor()) {
      // Arena invocations share one bounded physical pool, and every jobserver
      // unit must hold a slot. Submit the caller's share as well so concurrent
      // invoking threads cannot bypass either limit.
      while (NumWorkers--)
        TG.spawn(Worker);
    } else {
      // Run one worker on the calling thread: starts working immediately and
      // avoids an idle thread.
      while (--NumWorkers)
        TG.spawn(Worker);
      Worker();
    }
    return;
  }
#endif

  for (; Begin != End; ++Begin)
    Fn(Begin);
}
