//===- StaticArenaDeathTest.cpp - Arena fail-closed tests -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/StaticArena.h"
#include "llvm/Support/ToolExecutionContext.h"
#include "gtest/gtest.h"

#include <cstdint>
#include <memory>

using namespace llvm;

namespace {

static const uint32_t TemplateValue = 19;
static LLVMStaticArenaVarV1 ResolverRecord = {UINT64_MAX, sizeof(uint32_t),
                                              alignof(uint32_t), &TemplateValue,
                                              "StaticArenaDeathTest.Resolver"};

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
#elif defined(__ELF__) || defined(__wasm__)
#define STATIC_ARENA_ENTRY(Name, Record)                                       \
  extern "C" __attribute__((section("llvma_v1"), used))                        \
  LLVMStaticArenaVarV1 *const Name = &(Record)
#else
#define STATIC_ARENA_ENTRY(Name, Record)
#endif

STATIC_ARENA_ENTRY(LLVMStaticArenaDeathTestEntry, ResolverRecord);

#undef STATIC_ARENA_ENTRY
#if defined(_MSC_VER)
#undef STATIC_ARENA_FORCE_INCLUDE
#endif

namespace {

TEST(StaticArenaDeathTest, ResolverRejectsMissingBinding) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#elif !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  EXPECT_DEATH(
      {
        std::unique_ptr<StaticArena> Arena = StaticArena::create();
        (void)__llvm_arena_addr_v1(&ResolverRecord);
      },
      "no invocation is attached");
#endif
}

TEST(StaticArenaDeathTest, ClosingAllowsAccessButFinalizedRejectsIt) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#elif !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  {
    ScopedStaticArenaBinding Binding(*Arena);
    Arena->beginClosing();
    EXPECT_EQ(19u,
              *static_cast<uint32_t *>(__llvm_arena_addr_v1(&ResolverRecord)));
    Arena->runDestructors();
    EXPECT_DEATH((void)__llvm_arena_addr_v1(&ResolverRecord), "inactive arena");
  }
#endif
}

TEST(StaticArenaDeathTest, ResolverRejectsUnassignedAndOutOfBoundsRecords) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#elif !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  {
    ScopedStaticArenaBinding Binding(*Arena);
    LLVMStaticArenaVarV1 Unassigned = {UINT64_MAX, 1, 1, nullptr, "unassigned"};
    LLVMStaticArenaVarV1 PastEnd = {UINT64_MAX - 1, 1, 1, nullptr, "past-end"};
    LLVMStaticArenaVarV1 Oversized = {0, UINT64_MAX, 1, nullptr, "oversized"};

    EXPECT_DEATH((void)__llvm_arena_addr_v1(&Unassigned),
                 "unassigned or out of bounds");
    EXPECT_DEATH((void)__llvm_arena_addr_v1(&PastEnd),
                 "unassigned or out of bounds");
    EXPECT_DEATH((void)__llvm_arena_addr_v1(&Oversized),
                 "unassigned or out of bounds");

    Arena->beginClosing();
    Arena->runDestructors();
  }
#endif
}

TEST(StaticArenaDeathTest, CreateRevalidatesRecordsBeforeTemplateCopy) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#elif !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  EXPECT_DEATH(
      {
        std::unique_ptr<StaticArena> Arena = StaticArena::create();
        {
          ScopedStaticArenaBinding Binding(*Arena);
          Arena->beginClosing();
          Arena->runDestructors();
        }
        Arena.reset();

        // Model a record that became visible after the process-wide layout
        // was fixed. create() must reject it before using the unassigned
        // offset as a template-copy destination.
        ResolverRecord.Offset = UINT64_MAX;
        (void)StaticArena::create();
      },
      "unassigned or out of bounds");
#endif
}

TEST(StaticArenaDeathTest, OutstandingTaskLeasePreventsClosing) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#elif !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  EXPECT_DEATH(
      {
        std::unique_ptr<StaticArena> Arena = StaticArena::create();
        ScopedStaticArenaBinding Binding(*Arena);
        StaticArenaToken QueuedWork = StaticArenaToken::capture();
        (void)QueuedWork;
        Arena->beginClosing();
      },
      "not owner-bound and quiescent");
#endif
}

TEST(StaticArenaDeathTest, ClosingRejectsNewTaskCapture) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#elif !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  std::unique_ptr<StaticArena> Arena = StaticArena::create();
  {
    ScopedStaticArenaBinding Binding(*Arena);
    cl::ScopedContext CommandLineContext;
    Arena->beginClosing();
    EXPECT_DEATH((void)StaticArenaToken::capture(), "capturing a closing");
    EXPECT_DEATH((void)ToolExecutionContext::capture(), "capturing a closing");
    CommandLineContext.beginClosing();
    Arena->runDestructors();
  }
#endif
}

} // namespace
