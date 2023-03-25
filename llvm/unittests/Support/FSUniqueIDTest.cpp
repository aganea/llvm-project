//===- llvm/unittest/Support/FSUniqueIDTest.cpp - Test sys::fs::UniqueID --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/FileSystem/UniqueID.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::sys::fs;

namespace {

TEST(FSUniqueIDTest, construct) {
  EXPECT_EQ(20u, UniqueID::inMemory(20, 10).getDevice());
  EXPECT_EQ(10u, UniqueID::inMemory(20, 10).getFile());
}

TEST(FSUniqueIDTest, equals) {
  EXPECT_EQ(UniqueID::inMemory(20, 10), UniqueID::inMemory(20, 10));
  EXPECT_NE(UniqueID::inMemory(20, 20), UniqueID::inMemory(20, 10));
  EXPECT_NE(UniqueID::inMemory(10, 10), UniqueID::inMemory(20, 10));
}

TEST(FSUniqueIDTest, less) {
  EXPECT_FALSE(UniqueID::inMemory(20, 2) < UniqueID::inMemory(20, 2));
  EXPECT_FALSE(UniqueID::inMemory(20, 3) < UniqueID::inMemory(20, 2));
  EXPECT_FALSE(UniqueID::inMemory(30, 2) < UniqueID::inMemory(20, 2));
  EXPECT_FALSE(UniqueID::inMemory(30, 2) < UniqueID::inMemory(20, 40));
  EXPECT_TRUE(UniqueID::inMemory(20, 2) < UniqueID::inMemory(20, 3));
  EXPECT_TRUE(UniqueID::inMemory(20, 2) < UniqueID::inMemory(30, 2));
  EXPECT_TRUE(UniqueID::inMemory(20, 40) < UniqueID::inMemory(30, 2));
}

} // anonymous namespace
