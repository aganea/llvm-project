//===- PassRegistryTest.cpp ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/PassRegistry.h"
#include "llvm/PassInfo.h"
#include "llvm/Support/CommandLine.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

TEST(PassRegistryTest, LateCatalogRegistrationAllowedForOneInvocation) {
  PassRegistry Registry;
  int PassID;
  PassInfo Info("test pass", "test-pass", &PassID, nullptr, false, false);

  cl::ScopedContext Context;
  Registry.registerPass(Info);
  EXPECT_EQ(&Info, Registry.getPassInfo(&PassID));
}

TEST(PassRegistryTest, LateCatalogRegistrationRejectedAcrossInvocations) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#else
  EXPECT_DEATH(
      {
        PassRegistry Registry;
        int PassID;
        PassInfo Info("late pass", "late-pass", &PassID, nullptr, false,
                      false);
        cl::ScopedContext FirstContext;
        cl::ScopedContext SecondContext;
        Registry.registerPass(Info);
      },
      "pass registry.*complete process-wide registration before concurrent "
      "tool execution");
#endif
}

} // namespace
