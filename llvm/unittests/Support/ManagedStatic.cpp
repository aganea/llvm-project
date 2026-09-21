//===- llvm/unittest/Support/ManagedStatic.cpp - ManagedStatic tests ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/ManagedStatic.h"
#include "llvm/Config/config.h"
#include "llvm/Config/llvm-config.h" // for LLVM_ENABLE_THREADS
#include "llvm/Support/Allocator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/StaticArena.h"
#include "gtest/gtest.h"
#include <memory>
#include <thread>
#include <vector>
#ifdef HAVE_PTHREAD_H
#include <pthread.h>
#endif

using namespace llvm;

namespace {

#if LLVM_ENABLE_THREADS != 0 && defined(HAVE_PTHREAD_H) &&                     \
    !__has_feature(memory_sanitizer)
namespace test1 {
llvm::ManagedStatic<int> ms;
void *helper(void *) {
  *ms;
  return nullptr;
}

// Valgrind's leak checker complains glibc's stack allocation.
// To appease valgrind, we provide our own stack for each thread.
void *allocate_stack(pthread_attr_t &a, size_t n = 65536) {
  void *stack = safe_malloc(n);
  pthread_attr_init(&a);
#if defined(__linux__)
  pthread_attr_setstack(&a, stack, n);
#endif
  return stack;
}
} // namespace test1

TEST(Initialize, MultipleThreads) {
  // Run this test under tsan: http://code.google.com/p/data-race-test/

  pthread_attr_t a1, a2;
  void *p1 = test1::allocate_stack(a1);
  void *p2 = test1::allocate_stack(a2);

  pthread_t t1, t2;
  pthread_create(&t1, &a1, test1::helper, nullptr);
  pthread_create(&t2, &a2, test1::helper, nullptr);
  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);
  free(p1);
  free(p2);
}
#endif

namespace NestedStatics {
static ManagedStatic<int> Ms1;
struct Nest {
  Nest() { ++(*Ms1); }

  ~Nest() {
    assert(Ms1.isConstructed());
    ++(*Ms1);
  }
};
static ManagedStatic<Nest> Ms2;

TEST(ManagedStaticTest, NestedStatics) {
  EXPECT_FALSE(Ms1.isConstructed());
  EXPECT_FALSE(Ms2.isConstructed());

  *Ms2;
  EXPECT_TRUE(Ms1.isConstructed());
  EXPECT_TRUE(Ms2.isConstructed());
}
} // namespace NestedStatics

namespace CustomCreatorDeletor {
struct CustomCreate {
  static void *call() {
    void *Mem = safe_malloc(sizeof(int));
    *((int *)Mem) = 42;
    return Mem;
  }
};
struct CustomDelete {
  static void call(void *P) { std::free(P); }
};
static ManagedStatic<int, CustomCreate, CustomDelete> Custom;
TEST(ManagedStaticTest, CustomCreatorDeletor) { EXPECT_EQ(42, *Custom); }
} // namespace CustomCreatorDeletor

namespace ContextManagedStatics {
struct Value {
  int Number = 0;
};

static ContextManagedStatic<Value> PerContext;

TEST(ContextManagedStaticTest, NestedContextsAreIndependent) {
  Value *OuterPtr = nullptr;
  {
    cl::ScopedContext Outer;
    EXPECT_FALSE(PerContext.isConstructed());
    PerContext->Number = 11;
    OuterPtr = &*PerContext;
    EXPECT_TRUE(cl::isCurrentInvocationOwned(OuterPtr));

    {
      cl::ScopedContext Inner;
      EXPECT_FALSE(PerContext.isConstructed());
      PerContext->Number = 22;
      EXPECT_NE(OuterPtr, &*PerContext);
      EXPECT_EQ(22, PerContext->Number);
      EXPECT_FALSE(cl::isCurrentInvocationOwned(OuterPtr));
    }

    EXPECT_EQ(OuterPtr, &*PerContext);
    EXPECT_EQ(11, PerContext->Number);
  }

  EXPECT_FALSE(PerContext.isConstructed());
  EXPECT_FALSE(cl::isCurrentInvocationOwned(OuterPtr));
}

TEST(ContextManagedStaticTest, PropagatedWorkerUsesOwningContext) {
  cl::ScopedContext Context;
  PerContext->Number = 7;
  Value *OwnerPtr = &*PerContext;
  Value *WorkerPtr = nullptr;
  cl::ContextToken Token = cl::ContextToken::capture();

  std::thread Worker([Token = std::move(Token), &WorkerPtr]() mutable {
    cl::ScopedContextTokenBinding Binding(std::move(Token));
    WorkerPtr = &*PerContext;
    EXPECT_EQ(7, PerContext->Number);
  });
  Worker.join();

  EXPECT_EQ(OwnerPtr, WorkerPtr);
}

static std::vector<int> &destructionOrder() {
  static std::vector<int> Order;
  return Order;
}

template <int ID> struct OrderedValue {
  ~OrderedValue() { destructionOrder().push_back(ID); }
};

static ContextManagedStatic<OrderedValue<1>> First;
static ContextManagedStatic<OrderedValue<2>> Second;

TEST(ContextManagedStaticTest, DestroysInReverseConstructionOrder) {
  destructionOrder().clear();
  {
    cl::ScopedContext Context;
    (void)*First;
    (void)*Second;
  }
  EXPECT_EQ((std::vector<int>{2, 1}), destructionOrder());
}

struct SelfLookingValue {
  ~SelfLookingValue();
};

static ContextManagedStatic<SelfLookingValue> SelfLooking;
static SelfLookingValue *ObservedSelf;
static bool ObservedSelfOwned;

SelfLookingValue::~SelfLookingValue() {
  ObservedSelf = SelfLooking.isConstructed() ? &*SelfLooking : nullptr;
  ObservedSelfOwned = cl::isCurrentInvocationOwned(this);
}

TEST(ContextManagedStaticTest, RemainsPublishedDuringDestruction) {
  ObservedSelf = nullptr;
  ObservedSelfOwned = false;
  SelfLookingValue *Original = nullptr;
  {
    cl::ScopedContext Context;
    Original = &*SelfLooking;
  }
  EXPECT_EQ(Original, ObservedSelf);
  EXPECT_TRUE(ObservedSelfOwned);
}

TEST(ContextManagedStaticTest, ClaimTransfersCurrentInstance) {
  cl::ScopedContext Context;
  PerContext->Number = 31;
  std::unique_ptr<Value> Claimed(PerContext.claim());
  ASSERT_NE(nullptr, Claimed);
  EXPECT_EQ(31, Claimed->Number);
  EXPECT_FALSE(PerContext.isConstructed());
}

struct ContextLocatedOption {
  int Location = 0;
  cl::opt<int, true> Option;

  ContextLocatedOption()
      : Option("context-owned-location", cl::location(Location)) {}
};

static ContextManagedStatic<ContextLocatedOption> ContextLocated;

TEST(ContextManagedStaticTest, AcceptsContextOwnedOptionAndLocation) {
  cl::ScopedContext Context;
  ContextLocatedOption &Owned = *ContextLocated;
  EXPECT_TRUE(cl::isCurrentInvocationOwned(&Owned.Option));
  EXPECT_TRUE(cl::isCurrentInvocationOwned(&Owned.Location));
  Owned.Option = 23;
  EXPECT_EQ(23, Owned.Location);
}

static int ProcessLocation;

struct ProcessLocatedOption {
  cl::opt<int, true> Option;

  ProcessLocatedOption()
      : Option("context-option-process-location",
               cl::location(ProcessLocation)) {}
};

static ContextManagedStatic<ProcessLocatedOption> ProcessLocated;

TEST(ContextManagedStaticTest, NoArenaProcessLocationIsLifetimeSafeButShared) {
  {
    cl::ScopedContext Context;
    ProcessLocatedOption &Owned = *ProcessLocated;
    EXPECT_TRUE(cl::isCurrentInvocationOwned(&Owned.Option));
    EXPECT_FALSE(cl::isCurrentInvocationOwned(&ProcessLocation));
    Owned.Option = 41;
    EXPECT_EQ(41, ProcessLocation);
  }

  // With no arena, the context-owned option cannot outlive this process
  // location, so the pointer is lifetime-safe. The value is deliberately not
  // isolated, however; callers using this fallback must serialize and cannot
  // claim concurrent invocation support.
  EXPECT_EQ(41, ProcessLocation);
  ProcessLocation = 0;
}

} // namespace ContextManagedStatics

} // anonymous namespace
