//===- ProcessWideRegistryTest.cpp ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/ProcessWideRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Registry.h"
#include "gtest/gtest.h"

#include <array>
#include <atomic>
#include <thread>
#include <vector>

namespace process_wide_registry_test {
struct Plugin {
  virtual ~Plugin() = default;
};
struct LatePlugin : Plugin {};
using TestRegistry = llvm::Registry<Plugin>;
} // namespace process_wide_registry_test

LLVM_DECLARE_REGISTRY(process_wide_registry_test::TestRegistry)
LLVM_DEFINE_REGISTRY(process_wide_registry_test::TestRegistry)

TEST(ProcessWideRegistryTest, PluginRegistrationAllowedInOneInvocation) {
  llvm::cl::ScopedContext Context;
  static process_wide_registry_test::TestRegistry::Add<
      process_wide_registry_test::LatePlugin>
      Registration("single-invocation", "single-invocation plugin");

  auto Entries = process_wide_registry_test::TestRegistry::entries();
  EXPECT_NE(Entries.end(),
            llvm::find_if(Entries, [](const auto &Entry) {
              return Entry.getName() == "single-invocation";
            }));
}

TEST(ProcessWideRegistryTest, ConcurrentStartupMutationsAreSerialized) {
  // The registrations intentionally have process lifetime, like registrations
  // supplied by loaded plugins. Without one lock spanning both the policy
  // check and linked-list write, racing additions can lose or corrupt nodes.
  static constexpr std::array<const char *, 16> Names = {
      "concurrent-00", "concurrent-01", "concurrent-02", "concurrent-03",
      "concurrent-04", "concurrent-05", "concurrent-06", "concurrent-07",
      "concurrent-08", "concurrent-09", "concurrent-10", "concurrent-11",
      "concurrent-12", "concurrent-13", "concurrent-14", "concurrent-15"};
  using PluginRegistration = process_wide_registry_test::TestRegistry::Add<
      process_wide_registry_test::LatePlugin>;
  static std::array<std::unique_ptr<PluginRegistration>, Names.size()>
      Registrations;
  std::atomic<unsigned> Ready{0};
  std::atomic<bool> Start{false};
  std::vector<std::thread> Threads;
  Threads.reserve(Names.size());
  for (size_t I = 0; I != Names.size(); ++I)
    Threads.emplace_back([&, I] {
      Ready.fetch_add(1, std::memory_order_release);
      while (!Start.load(std::memory_order_acquire))
        std::this_thread::yield();
      if (!Registrations[I])
        Registrations[I] =
            std::make_unique<PluginRegistration>(Names[I], "concurrent plugin");
    });

  while (Ready.load(std::memory_order_acquire) != Names.size())
    std::this_thread::yield();
  Start.store(true, std::memory_order_release);
  for (std::thread &Thread : Threads)
    Thread.join();

  unsigned Found = 0;
  for (const auto &Entry : process_wide_registry_test::TestRegistry::entries())
    if (Entry.getName().starts_with("concurrent-"))
      ++Found;
  EXPECT_EQ(Names.size(), Found);
}

TEST(ProcessWideRegistryTest, CannotAddPluginAcrossInvocations) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#else
  EXPECT_DEATH(
      {
        llvm::cl::ScopedContext FirstContext;
        llvm::cl::ScopedContext SecondContext;
        process_wide_registry_test::TestRegistry::Add<
            process_wide_registry_test::LatePlugin>
            Registration("late", "late plugin");
      },
      "complete process-wide registration before concurrent tool execution");
#endif
}

TEST(ProcessWideRegistryTest, FreezeRejectsMutationWithoutContext) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#else
  EXPECT_DEATH(
      {
        llvm::ProcessWideRegistryFreeze Freeze;
        process_wide_registry_test::TestRegistry::Add<
            process_wide_registry_test::LatePlugin>
            Registration("late-frozen", "late frozen plugin");
      },
      "process-wide plugin registry.*registration was frozen");
#endif
}

TEST(ProcessWideRegistryTest, FreezeRejectsForeignThreadMutation) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#else
  EXPECT_DEATH(
      {
        llvm::cl::ScopedContext Context;
        llvm::ProcessWideRegistryFreeze Freeze;
        std::thread ForeignThread([] {
          process_wide_registry_test::TestRegistry::Add<
              process_wide_registry_test::LatePlugin>
              Registration("foreign-late", "foreign late plugin");
        });
        ForeignThread.join();
      },
      "process-wide plugin registry.*registration was frozen");
#endif
}

TEST(ProcessWideRegistryTest, ExclusiveServiceAllowsStandaloneOrOneContext) {
  {
    llvm::ExclusiveProcessServiceLease Lease("standalone test service");
  }
  {
    llvm::cl::ScopedContext Context;
    llvm::ExclusiveProcessServiceLease Lease("single-context test service");
  }
}

TEST(ProcessWideRegistryTest, CannotAcquireExclusiveServiceAcrossInvocations) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#else
  EXPECT_DEATH(
      {
        llvm::cl::ScopedContext FirstContext;
        llvm::cl::ScopedContext SecondContext;
        llvm::ExclusiveProcessServiceLease Lease("test crash service");
      },
      "test crash service.*multiple tool invocations");
#endif
}

TEST(ProcessWideRegistryTest, CannotStartInvocationWithExclusiveService) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#else
  EXPECT_DEATH(
      {
        llvm::ExclusiveProcessServiceLease Lease("test crash service");
        llvm::cl::ScopedContext Context;
      },
      "cannot start a tool invocation while an exclusive process service");
#endif
}

TEST(ProcessWideRegistryTest, CannotTearDownInvocationWithExclusiveService) {
#if !GTEST_HAS_DEATH_TEST
  GTEST_SKIP() << "death tests are unavailable";
#else
  EXPECT_DEATH(
      {
        auto *Context = new llvm::cl::ScopedContext();
        llvm::ExclusiveProcessServiceLease Lease("test crash service");
        delete Context;
      },
      "cannot tear down a tool invocation while an exclusive process service");
#endif
}
