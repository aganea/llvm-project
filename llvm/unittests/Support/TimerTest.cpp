//===- unittests/TimerTest.cpp - Timer tests ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Timer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

#if _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include <memory>

using namespace llvm;

namespace {

// FIXME: Put this somewhere in Support, it's also used in LockFileManager.
void SleepMS() {
#if _WIN32
  Sleep(1);
#else
  struct timespec Interval;
  Interval.tv_sec = 0;
  Interval.tv_nsec = 1000000;
#if defined(__MVS__)
  long Microseconds = (Interval.tv_nsec + 999) / 1000;
  usleep(Microseconds);
#else
  nanosleep(&Interval, nullptr);
#endif
#endif
}

TEST(Timer, Additivity) {
  Timer T1("T1", "T1");

  EXPECT_TRUE(T1.isInitialized());

  T1.startTimer();
  T1.stopTimer();
  auto TR1 = T1.getTotalTime();

  T1.startTimer();
  SleepMS();
  T1.stopTimer();
  auto TR2 = T1.getTotalTime();

  EXPECT_LT(TR1, TR2);
}

TEST(Timer, CheckIfTriggered) {
  Timer T1("T1", "T1");

  EXPECT_FALSE(T1.hasTriggered());
  T1.startTimer();
  EXPECT_TRUE(T1.hasTriggered());
  T1.stopTimer();
  EXPECT_TRUE(T1.hasTriggered());

  T1.clear();
  EXPECT_FALSE(T1.hasTriggered());
}

TEST(Timer, TimerGroupTimerDestructed) {
  testing::internal::CaptureStderr();

  {
    TimerGroup TG("tg", "desc");
    {
      Timer T1("T1", "T1", TG);
      T1.startTimer();
      T1.stopTimer();
    }
    EXPECT_TRUE(testing::internal::GetCapturedStderr().empty());
    testing::internal::CaptureStderr();
  }
  EXPECT_FALSE(testing::internal::GetCapturedStderr().empty());
}

TEST(Timer, TimerGroupsAreInvocationLocal) {
  std::string OuterOutput;
  cl::ScopedContext OuterContext;
  TimerGroup OuterGroup("outer", "outer timer group", false);
  Timer OuterTimer("outer-timer", "outer timer", OuterGroup);
  OuterTimer.startTimer();
  OuterTimer.stopTimer();

  {
    std::string InnerOutput;
    cl::ScopedContext InnerContext;
    TimerGroup InnerGroup("inner", "inner timer group", false);
    Timer InnerTimer("inner-timer", "inner timer", InnerGroup);
    InnerTimer.startTimer();
    InnerTimer.stopTimer();

    raw_string_ostream OS(InnerOutput);
    TimerGroup::printAll(OS);
    EXPECT_NE(InnerOutput.find("inner timer group"), std::string::npos);
    EXPECT_EQ(InnerOutput.find("outer timer group"), std::string::npos);
  }

  raw_string_ostream OS(OuterOutput);
  TimerGroup::printAll(OS);
  EXPECT_NE(OuterOutput.find("outer timer group"), std::string::npos);
  EXPECT_EQ(OuterOutput.find("inner timer group"), std::string::npos);
}

TEST(Timer, TimerGroupCanOutliveInvocationState) {
  std::unique_ptr<TimerGroup> Group;
  {
    cl::ScopedContext Context;
    Group = std::make_unique<TimerGroup>("survivor", "surviving group", false);
  }

  // Process-lifetime TimerGroups are destroyed after llvm_shutdown(). Their
  // list and lock state must therefore remain alive until the last group.
  Group.reset();
}

} // namespace
