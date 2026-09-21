//===- PrintCrashIRInstrumentationTest.cpp -------------------------------===//
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
class PrintCrashIRInstrumentationTest : public testing::Test {
protected:
  void SetUp() override {
    cl::ResetAllOptionOccurrences();
    const char *Args[] = {"print-crash-ir-test", "-print-on-crash"};
    ASSERT_TRUE(cl::ParseCommandLineOptions(std::size(Args), Args, StringRef(),
                                            &nulls()));
  }

  void TearDown() override { cl::ResetAllOptionOccurrences(); }
};
} // namespace

TEST_F(PrintCrashIRInstrumentationTest, ReusesOneSignalHandlerAcrossRuns) {
  // AddSignalHandler has eight process-wide slots. Registering one callback per
  // successful invocation would fail before this loop completes.
  for (unsigned I = 0; I != 16; ++I) {
    PassInstrumentationCallbacks PIC;
    PrintCrashIRInstrumentation Reporter;
    Reporter.registerCallbacks(PIC);
  }
}
