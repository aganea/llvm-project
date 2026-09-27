//===-- Statistic.cpp - Easy way to expose stats information --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the 'Statistic' class, which is designed to be an easy
// way to expose various success metrics from passes.  These statistics are
// printed at the end of a run, when the -stats command line option is enabled
// on the command line.
//
// This is useful for reporting information like the number of instructions
// simplified, optimized or removed by various transformations, like this:
//
// static Statistic NumInstEliminated("GCSE", "Number of instructions killed");
//
// Later, in the code: ++NumInstEliminated;
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"

#include "DebugOptions.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstring>
using namespace llvm;

namespace {
struct StatisticSettings {
  bool Enabled = false;
  bool PrintOnExit = false;

  // Keep option values with their owning command-line context. In particular,
  // StatisticInfo is destroyed after an invocation's StaticArena has already
  // been finalized, so its destructor must not resolve arena-backed globals.
  bool CommandLineEnabled = false;
  bool StatsAsJSON = false;
  cl::opt<bool, true> EnableStatsOption{
      "stats",
      cl::desc(
          "Enable statistics output from program (available with Asserts)"),
      cl::location(CommandLineEnabled), cl::Hidden};
  cl::opt<bool, true> StatsAsJSONOption{
      "stats-json", cl::desc("Display statistics as json data"),
      cl::location(StatsAsJSON), cl::Hidden};
};
static ContextManagedStatic<StatisticSettings> Settings;
} // namespace

void llvm::initStatisticOptions() { (void)*Settings; }

namespace {
/// This class is used in a ContextManagedStatic so that it is created on demand
/// (when the first statistic is bumped) and destroyed with its owning command
/// line context. We print statistics from the destructor.
/// This class is also used to look up statistic values from applications that
/// use LLVM.
class StatisticInfo {
  struct TouchedStatistic {
    TrackingStatistic *Stat;
    // Arena-owned statistics may already be finalized at context teardown.
    bool ResetOnTeardown;
  };

  std::vector<TrackingStatistic *> Stats;
  std::vector<TouchedStatistic> TouchedStats;

  friend void llvm::PrintStatistics();
  friend void llvm::PrintStatistics(raw_ostream &OS);
  friend void llvm::PrintStatisticsJSON(raw_ostream &OS);

  /// Sort statistics by debugtype,name,description.
  void sort();

public:
  using const_iterator = std::vector<TrackingStatistic *>::const_iterator;

  StatisticInfo();
  ~StatisticInfo();

  void addStatistic(TrackingStatistic *S, bool Enabled, bool ResetOnTeardown) {
    TouchedStats.push_back({S, ResetOnTeardown});
    if (Enabled)
      Stats.push_back(S);
  }

  const_iterator begin() const { return Stats.begin(); }
  const_iterator end() const { return Stats.end(); }
  iterator_range<const_iterator> statistics() const { return {begin(), end()}; }

  void reset();
};
} // end anonymous namespace

static ContextManagedStatic<StatisticInfo> StatInfo;
static ContextManagedStatic<sys::SmartMutex<true>> StatLock;

/// RegisterStatistic - The first time a statistic is bumped, this method is
/// called.
void TrackingStatistic::RegisterStatistic() {
  // Track this statistic in the current context, even when collection is
  // disabled, so process-owned state can be reset at teardown.
  // Context teardown can end up calling PrintStatistics, which takes StatLock.
  // Dereference the context statics before taking StatLock so first
  // construction never inverts that lock ordering.
  if (!Initialized.load(std::memory_order_relaxed)) {
    // Classify before taking StatLock to preserve the context-static lock
    // ordering. Only process-owned statistics survive this context's teardown.
    const bool ResetOnTeardown = !cl::isCurrentInvocationOwned(this);
    sys::SmartMutex<true> &Lock = *StatLock;
    StatisticInfo &SI = *StatInfo;
    sys::SmartScopedLock<true> Writer(Lock);
    // Check Initialized again after acquiring the lock.
    if (Initialized.load(std::memory_order_relaxed))
      return;
    SI.addStatistic(this, Settings->CommandLineEnabled || Settings->Enabled,
                    ResetOnTeardown);

    // Remember we have been registered.
    Initialized.store(true, std::memory_order_release);
  }
}

StatisticInfo::StatisticInfo() {
  // These are used by the destructor. Construct them before this instance is
  // published so reverse-order context teardown keeps both alive until the
  // destructor returns. StatisticInfo can otherwise be created by an API call
  // before command-line parsing constructs Settings.
  (void)*Settings;
  (void)*StatLock;

  // Ensure that necessary timer global objects are created first so they are
  // destructed after us.
  TimerGroup::constructForStatistics();
}

// Print information when destroyed, iff command line option is specified.
StatisticInfo::~StatisticInfo() {
  if (Settings->CommandLineEnabled || Settings->PrintOnExit)
    llvm::PrintStatistics();

  // Unlowered globals retain their registration bit and count between
  // invocations. Reset every process-owned statistic touched here, including
  // those touched while collection was disabled.
  sys::SmartScopedLock<true> Writer(*StatLock);
  for (const TouchedStatistic &Touched : TouchedStats) {
    if (!Touched.ResetOnTeardown)
      continue;
    Touched.Stat->Initialized = false;
    Touched.Stat->Value = 0;
  }
}

void llvm::EnableStatistics(bool DoPrintOnExit) {
  Settings->Enabled = true;
  Settings->PrintOnExit = DoPrintOnExit;
}

bool llvm::AreStatisticsEnabled() {
  return Settings->Enabled || Settings->CommandLineEnabled;
}

void StatisticInfo::sort() {
  llvm::stable_sort(
      Stats, [](const TrackingStatistic *LHS, const TrackingStatistic *RHS) {
        if (int Cmp = std::strcmp(LHS->getDebugType(), RHS->getDebugType()))
          return Cmp < 0;

        if (int Cmp = std::strcmp(LHS->getName(), RHS->getName()))
          return Cmp < 0;

        return std::strcmp(LHS->getDesc(), RHS->getDesc()) < 0;
      });
}

void StatisticInfo::reset() {
  sys::SmartScopedLock<true> Writer(*StatLock);

  // Tell each statistic that it isn't registered so it has to register
  // again. We're holding the lock so it won't be able to do so until we're
  // finished. Once we've forced it to re-register (after we return), then zero
  // the value.
  for (const TouchedStatistic &Touched : TouchedStats) {
    // Value updates to a statistic that complete before this statement in the
    // iteration for that statistic will be lost as intended.
    Touched.Stat->Initialized = false;
    Touched.Stat->Value = 0;
  }

  // Clear the registration list and release the lock once we're done. Any
  // pending updates from other threads will safely take effect after we return.
  // That might not be what the user wants if they're measuring a compilation
  // but it's their responsibility to prevent concurrent compilations to make
  // a single compilation measurable.
  Stats.clear();
  TouchedStats.clear();
}

void llvm::PrintStatistics(raw_ostream &OS) {
  StatisticInfo &Stats = *StatInfo;

  // Figure out how long the biggest Value and Name fields are.
  unsigned MaxDebugTypeLen = 0, MaxValLen = 0;
  for (TrackingStatistic *Stat : Stats.Stats) {
    MaxValLen = std::max(MaxValLen, (unsigned)utostr(Stat->getValue()).size());
    MaxDebugTypeLen =
        std::max(MaxDebugTypeLen, (unsigned)std::strlen(Stat->getDebugType()));
  }

  Stats.sort();

  // Print out the statistics header...
  OS << "===" << std::string(73, '-') << "===\n"
     << "                          ... Statistics Collected ...\n"
     << "===" << std::string(73, '-') << "===\n\n";

  // Print all of the statistics.
  for (TrackingStatistic *Stat : Stats.Stats)
    OS << format("%*" PRIu64 " %-*s - %s\n", MaxValLen, Stat->getValue(),
                 MaxDebugTypeLen, Stat->getDebugType(), Stat->getDesc());

  OS << '\n'; // Flush the output stream.
  OS.flush();
}

void llvm::PrintStatisticsJSON(raw_ostream &OS) {
  sys::SmartScopedLock<true> Reader(*StatLock);
  StatisticInfo &Stats = *StatInfo;

  Stats.sort();

  // Print all of the statistics.
  OS << "{\n";
  const char *delim = "";
  for (const TrackingStatistic *Stat : Stats.Stats) {
    OS << delim;
    assert(yaml::needsQuotes(Stat->getDebugType()) == yaml::QuotingType::None &&
           "Statistic group/type name is simple.");
    assert(yaml::needsQuotes(Stat->getName()) == yaml::QuotingType::None &&
           "Statistic name is simple");
    OS << "\t\"" << Stat->getDebugType() << '.' << Stat->getName()
       << "\": " << Stat->getValue();
    delim = ",\n";
  }
  // Print timers.
  TimerGroup::printAllJSONValues(OS, delim);

  OS << "\n}\n";
  OS.flush();
}

void llvm::PrintStatistics() {
#if LLVM_ENABLE_STATS
  sys::SmartScopedLock<true> Reader(*StatLock);
  StatisticInfo &Stats = *StatInfo;

  // Statistics not enabled?
  if (Stats.Stats.empty())
    return;

  // Get the stream to write to.
  std::unique_ptr<raw_ostream> OutStream = CreateInfoOutputFile();
  if (Settings->StatsAsJSON)
    PrintStatisticsJSON(*OutStream);
  else
    PrintStatistics(*OutStream);

#else
  // Check if the -stats option is set instead of checking
  // !Stats.Stats.empty().  In release builds, Statistics operators
  // do nothing, so stats are never Registered.
  if (Settings->CommandLineEnabled) {
    // Get the stream to write to.
    std::unique_ptr<raw_ostream> OutStream = CreateInfoOutputFile();
    (*OutStream) << "Statistics are disabled.  "
                 << "Build with asserts or with -DLLVM_FORCE_ENABLE_STATS\n";
  }
#endif
}

std::vector<std::pair<StringRef, uint64_t>> llvm::GetStatistics() {
  sys::SmartScopedLock<true> Reader(*StatLock);
  std::vector<std::pair<StringRef, uint64_t>> ReturnStats;

  for (const auto &Stat : StatInfo->statistics())
    ReturnStats.emplace_back(Stat->getName(), Stat->getValue());
  return ReturnStats;
}

void llvm::ResetStatistics() { StatInfo->reset(); }
