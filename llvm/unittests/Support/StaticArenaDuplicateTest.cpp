//===- StaticArenaDuplicateTest.cpp - Duplicate arena record test --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/StaticArena.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

static LLVMStaticArenaVarV1 DuplicateRecord = {
    UINT64_MAX, 4, 4, nullptr, "StaticArenaDuplicateTest.Record"};

} // namespace

#if defined(_MSC_VER)
#pragma section(".llvma$v1$b", read)
#if defined(_M_IX86)
#define STATIC_ARENA_FORCE_INCLUDE(Name)                                       \
  __pragma(comment(linker, "/include:_" #Name))
#else
#define STATIC_ARENA_FORCE_INCLUDE(Name)                                       \
  __pragma(comment(linker, "/include:" #Name))
#endif
#define STATIC_ARENA_ENTRY(Name, Record)                                       \
  extern "C" __declspec(allocate(".llvma$v1$b"))                               \
  LLVMStaticArenaVarV1 *const Name = &(Record);                                \
  STATIC_ARENA_FORCE_INCLUDE(Name)
#elif defined(_WIN32)
#define STATIC_ARENA_ENTRY(Name, Record)                                       \
  extern "C" __attribute__((section(".llvma$v1$b"), used))                     \
  LLVMStaticArenaVarV1 *const Name = &(Record)
#elif defined(__ELF__)
#define STATIC_ARENA_ENTRY(Name, Record)                                       \
  extern "C" __attribute__((section("llvma_v1"), used))                        \
  LLVMStaticArenaVarV1 *const Name = &(Record)
#else
#define STATIC_ARENA_ENTRY(Name, Record)
#endif

STATIC_ARENA_ENTRY(LLVMStaticArenaDuplicateTestEntryA, DuplicateRecord);
STATIC_ARENA_ENTRY(LLVMStaticArenaDuplicateTestEntryB, DuplicateRecord);

#undef STATIC_ARENA_ENTRY
#if defined(_MSC_VER)
#undef STATIC_ARENA_FORCE_INCLUDE
#endif

TEST(StaticArenaDuplicateTest, RejectsDuplicateRecordPointer) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#elif !defined(_WIN32) && !defined(__ELF__)
  GTEST_SKIP() << "the v1 producer supports COFF and ELF only";
#else
  EXPECT_DEATH((void)StaticArena::create(), "duplicate record");
#endif
}
