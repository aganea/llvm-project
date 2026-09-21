//===- PrintCrashIRInstrumentationDeathTest.cpp --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/PassInstrumentation.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

#include <iterator>

using namespace llvm;

namespace {
class PrintCrashIRInstrumentationDeathTest : public testing::Test {
protected:
  void SetUp() override {
    cl::ResetAllOptionOccurrences();
    const char *Args[] = {"print-crash-ir-death-test", "-print-on-crash"};
    ASSERT_TRUE(cl::ParseCommandLineOptions(std::size(Args), Args, StringRef(),
                                            &nulls()));
  }

  void TearDown() override { cl::ResetAllOptionOccurrences(); }
};
} // namespace

TEST_F(PrintCrashIRInstrumentationDeathTest,
       OverlappingInvocationsRejectCrashReporter) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#else
  EXPECT_DEATH(
      {
        cl::ScopedContext FirstContext;
        cl::ScopedContext SecondContext;
        PassInstrumentationCallbacks PIC;
        PrintCrashIRInstrumentation Reporter;
        Reporter.registerCallbacks(PIC);
      },
      "print-on-crash.*multiple tool invocations");
#endif
}

TEST_F(PrintCrashIRInstrumentationDeathTest,
       NonOwnerDestructionDoesNotReleaseCrashService) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#else
  PassInstrumentationCallbacks OwnerPIC;
  PrintCrashIRInstrumentation Owner;
  Owner.registerCallbacks(OwnerPIC);

  {
    PassInstrumentationCallbacks NonOwnerPIC;
    PrintCrashIRInstrumentation NonOwner;
    NonOwner.registerCallbacks(NonOwnerPIC);
  }

  EXPECT_DEATH(
      { cl::ScopedContext OverlappingContext; },
      "cannot start a tool invocation while an exclusive process service");
#endif
}
