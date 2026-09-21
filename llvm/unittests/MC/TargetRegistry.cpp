//===- unittests/MC/TargetRegistry.cpp ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// The target registry code lives in Support, but it relies on linking in all
// LLVM targets. We keep this test with the MC tests, which already do that, to
// keep the SupportTests target small.

#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ProcessWideRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "gtest/gtest.h"

#include <atomic>
#include <thread>

using namespace llvm;

namespace {

MCInstrInfo *createTestMCInstrInfo() {
  static int Token;
  return reinterpret_cast<MCInstrInfo *>(&Token);
}
MCInstrInfo *createOtherMCInstrInfo() {
  static int Token;
  return reinterpret_cast<MCInstrInfo *>(&Token);
}

TEST(TargetRegistry, TargetHasArchType) {
  // Presence of at least one target will be asserted when done with the loop,
  // else this would pass by accident if InitializeAllTargetInfos were omitted.
  int Count = 0;

  llvm::InitializeAllTargetInfos();

  for (const Target &T : TargetRegistry::targets()) {
    StringRef Name = T.getName();
    // There is really no way (at present) to ask a Target whether it targets
    // a specific architecture, because the logic for that is buried in a
    // predicate.
    // We can't ask the predicate "Are you a function that always returns
    // false?"
    // So given that the cpp backend truly has no target arch, it is skipped.
    if (Name != "cpp") {
      Triple::ArchType Arch = Triple::getArchTypeForLLVMName(Name);
      EXPECT_NE(Arch, Triple::UnknownArch);
      ++Count;
    }
  }
  ASSERT_NE(Count, 0);
}

TEST(TargetRegistry, IsValidFeatureListFormat) {
  // Valid strings

  // Empty string is a valid feature string
  EXPECT_TRUE(Target::isValidFeatureListFormat(""));

  EXPECT_TRUE(Target::isValidFeatureListFormat("+some_feature"));
  EXPECT_TRUE(Target::isValidFeatureListFormat("-some_feature"));
  EXPECT_TRUE(
      Target::isValidFeatureListFormat("+feature1,-feature2,+feature3"));
  EXPECT_TRUE(Target::isValidFeatureListFormat("+123"));

  // Strings with single trailing comma are also valid
  EXPECT_TRUE(Target::isValidFeatureListFormat("-feature,"));
  EXPECT_TRUE(
      Target::isValidFeatureListFormat("-feature1,+feature2,+feature3,"));

  // Invalid strings

  // Feature don't start with '+' or '-'
  EXPECT_FALSE(Target::isValidFeatureListFormat("invalid_string"));
  EXPECT_FALSE(Target::isValidFeatureListFormat("+good,bad"));
  EXPECT_FALSE(Target::isValidFeatureListFormat("bad,+good"));

  // String has spaces
  EXPECT_FALSE(Target::isValidFeatureListFormat(" "));
  EXPECT_FALSE(Target::isValidFeatureListFormat(", "));
  EXPECT_FALSE(Target::isValidFeatureListFormat(" avx"));
  EXPECT_FALSE(Target::isValidFeatureListFormat("+avx, -sse"));

  // Redundant commas
  EXPECT_FALSE(Target::isValidFeatureListFormat("+feature1,,+feature2"));
  EXPECT_FALSE(Target::isValidFeatureListFormat(",+feature"));
  EXPECT_FALSE(
      Target::isValidFeatureListFormat("+feature1,,,+feature2,,+feature3"));

  // Feature consists only of '+' or '-'
  EXPECT_FALSE(Target::isValidFeatureListFormat("+"));
  EXPECT_FALSE(Target::isValidFeatureListFormat("-"));
  EXPECT_FALSE(Target::isValidFeatureListFormat("+avx,-"));

  // Only commas
  EXPECT_FALSE(Target::isValidFeatureListFormat(","));
  EXPECT_FALSE(Target::isValidFeatureListFormat(",,"));
  EXPECT_FALSE(Target::isValidFeatureListFormat(",,,"));
}

TEST(TargetRegistry, ConcurrentRepeatedComponentRegistrationIsANoOp) {
  Target T{};
  TargetRegistry::RegisterMCInstrInfo(T, createTestMCInstrInfo);
  std::atomic<unsigned> Ready{0};

  auto RepeatRegistration = [&] {
    cl::ScopedContext Context;
    Ready.fetch_add(1, std::memory_order_release);
    while (Ready.load(std::memory_order_acquire) != 2)
      std::this_thread::yield();
    TargetRegistry::RegisterMCInstrInfo(T, createTestMCInstrInfo);
  };

  std::thread First(RepeatRegistration);
  std::thread Second(RepeatRegistration);
  First.join();
  Second.join();
}

TEST(TargetRegistry, CannotReplaceComponentAcrossInvocations) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#else
  EXPECT_DEATH(
      {
        Target T{};
        TargetRegistry::RegisterMCInstrInfo(T, createTestMCInstrInfo);
        cl::ScopedContext FirstContext;
        cl::ScopedContext SecondContext;
        TargetRegistry::RegisterMCInstrInfo(T, createOtherMCInstrInfo);
      },
      "complete process-wide registration before concurrent tool execution");
#endif
}

TEST(TargetRegistry, FrozenRegistryAllowsIdempotentComponentRegistration) {
  Target T{};
  TargetRegistry::RegisterMCInstrInfo(T, createTestMCInstrInfo);
  ProcessWideRegistryFreeze Freeze;
  TargetRegistry::RegisterMCInstrInfo(T, createTestMCInstrInfo);
}

TEST(TargetRegistry, FrozenRegistryRejectsComponentReplacement) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#else
  EXPECT_DEATH(
      {
        Target T{};
        TargetRegistry::RegisterMCInstrInfo(T, createTestMCInstrInfo);
        ProcessWideRegistryFreeze Freeze;
        TargetRegistry::RegisterMCInstrInfo(T, createOtherMCInstrInfo);
      },
      "target registry.*registration was frozen");
#endif
}

} // end namespace
