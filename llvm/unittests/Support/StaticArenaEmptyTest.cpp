//===- StaticArenaEmptyTest.cpp - Empty static-arena layout test ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/StaticArena.h"
#include "gtest/gtest.h"

#include <memory>

using namespace llvm;

TEST(StaticArenaEmptyTest, EmptyLayoutCanBeOwnedAndFinalized) {
#if !defined(_WIN32) && !defined(__ELF__)
  GTEST_SKIP() << "the v1 producer supports COFF and ELF only";
#else
  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  {
    ScopedStaticArenaBinding Binding(*Arena);
    int Outside = 0;
    EXPECT_FALSE(isInCurrentStaticArena(nullptr));
    EXPECT_FALSE(isInCurrentStaticArena(&Outside));
    Arena->beginClosing();
    Arena->runDestructors();
  }
#endif
}
