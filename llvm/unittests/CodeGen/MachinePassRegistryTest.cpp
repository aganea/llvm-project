//===- MachinePassRegistryTest.cpp ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/MachinePassRegistry.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ProcessWideRegistry.h"
#include "gtest/gtest.h"

#include <atomic>
#include <thread>

using namespace llvm;

namespace {

using TestCtor = int (*)();

int createFirst() { return 1; }
int createSecond() { return 2; }

class TestListener : public MachinePassRegistryListener<TestCtor> {
public:
  unsigned Adds = 0;
  unsigned Removes = 0;

  void NotifyAdd(StringRef, TestCtor, StringRef) override { ++Adds; }
  void NotifyRemove(StringRef) override { ++Removes; }
};

FunctionPass *createTestRegAlloc() { return nullptr; }

class TestRegAllocA : public RegisterRegAllocBase<TestRegAllocA> {
public:
  using RegisterRegAllocBase::RegisterRegAllocBase;
};

class TestRegAllocB : public RegisterRegAllocBase<TestRegAllocB> {
public:
  using RegisterRegAllocBase::RegisterRegAllocBase;
};

TEST(MachinePassRegistryTest, FreshContextsOwnDefaultsAndListeners) {
  MachinePassRegistry<TestCtor> Registry;
  MachinePassRegistryNode<TestCtor> Node("test", "test pass", createFirst);
  Registry.Add(&Node);

  {
    cl::ScopedContext Context;
    TestListener Listener;
    EXPECT_EQ(nullptr, Registry.getDefault());
    Registry.setDefault(createFirst);
    Registry.setListener(&Listener);
    EXPECT_EQ(createFirst, Registry.getDefault());
    EXPECT_EQ(&Node, Registry.getList());
    Registry.setListener(nullptr);
  }

  {
    cl::ScopedContext Context;
    TestListener Listener;
    EXPECT_EQ(nullptr, Registry.getDefault());
    Registry.setDefault(createSecond);
    Registry.setListener(&Listener);
    EXPECT_EQ(createSecond, Registry.getDefault());
    EXPECT_EQ(&Node, Registry.getList());
    Registry.setListener(nullptr);
  }

  Registry.Remove(&Node);
}

TEST(MachinePassRegistryTest, ConcurrentContextsDoNotShareSelection) {
  MachinePassRegistry<TestCtor> Registry;
  std::atomic<unsigned> Ready{0};

  auto Run = [&](TestCtor Ctor) {
    cl::ScopedContext Context;
    TestListener Listener;
    Registry.setListener(&Listener);
    Registry.setDefault(Ctor);
    Ready.fetch_add(1, std::memory_order_release);
    while (Ready.load(std::memory_order_acquire) != 2)
      std::this_thread::yield();
    EXPECT_EQ(Ctor, Registry.getDefault());
    Registry.setListener(nullptr);
  };

  std::thread First(Run, createFirst);
  std::thread Second(Run, createSecond);
  First.join();
  Second.join();
}

TEST(MachinePassRegistryTest, ConcurrentListenersFreezeRegistrationCatalog) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#else
  EXPECT_DEATH(
      {
        MachinePassRegistry<TestCtor> Registry;
        TestListener OuterListener;
        TestListener InnerListener;
        MachinePassRegistryNode<TestCtor> Node("late", "late pass",
                                               createFirst);
        cl::ScopedContext OuterContext;
        Registry.setListener(&OuterListener);
        cl::ScopedContext InnerContext;
        Registry.setListener(&InnerListener);
        Registry.Add(&Node);
      },
      "complete process-wide registration before concurrent tool execution");
#endif
}

TEST(MachinePassRegistryTest, ProcessFreezeRejectsLateRegistration) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#else
  EXPECT_DEATH(
      {
        MachinePassRegistry<TestCtor> Registry;
        MachinePassRegistryNode<TestCtor> Node("late-frozen", "late pass",
                                               createFirst);
        ProcessWideRegistryFreeze Freeze;
        Registry.Add(&Node);
      },
      "machine-pass registry.*registration was frozen");
#endif
}

TEST(MachinePassRegistryTest, RegAllocFamiliesHaveProcessWideCatalogs) {
  TestRegAllocA First("first", "first allocator", createTestRegAlloc);
  TestRegAllocA Second("second", "second allocator", createTestRegAlloc);
  TestRegAllocB Other("other", "other allocator", createTestRegAlloc);

  EXPECT_EQ(&Second, TestRegAllocA::getList());
  EXPECT_EQ(&First, Second.getNext());
  EXPECT_EQ(nullptr, First.getNext());
  EXPECT_EQ(&Other, TestRegAllocB::getList());
  EXPECT_EQ(nullptr, Other.getNext());

  {
    cl::ScopedContext Context;
    EXPECT_EQ(nullptr, TestRegAllocA::getDefault());
    TestRegAllocA::setDefault(createTestRegAlloc);
    EXPECT_EQ(createTestRegAlloc, TestRegAllocA::getDefault());
  }

  // Selection state is invocation-owned even though the catalog is not.
  cl::ScopedContext Context;
  EXPECT_EQ(nullptr, TestRegAllocA::getDefault());
}

} // namespace
