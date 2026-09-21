//===- ToolOutputFileTest.cpp - ToolOutputFile tests ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/FileSystem.h"
#include "gtest/gtest.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace llvm;

namespace {

TEST(ToolOutputFileTest, DashOpensOuts) {
  std::error_code EC;
  EXPECT_EQ(&ToolOutputFile("-", EC, sys::fs::OF_None).os(), &outs());
}

#ifdef _WIN32
TEST(ToolOutputFileTest, ConcurrentSignalCleanupRegistration) {
  constexpr unsigned ThreadCount = 8;
  constexpr unsigned Iterations = 16;
  std::atomic<unsigned> Ready{0};
  std::atomic<unsigned> Failures{0};
  std::atomic<bool> Start{false};
  std::vector<std::thread> Threads;

  for (unsigned I = 0; I != ThreadCount; ++I) {
    Threads.emplace_back([&] {
      Ready.fetch_add(1, std::memory_order_release);
      while (!Start.load(std::memory_order_acquire))
        std::this_thread::yield();

      for (unsigned J = 0; J != Iterations; ++J) {
        std::error_code EC;
        ToolOutputFile Output("NUL", EC, sys::fs::OF_None);
        if (EC)
          Failures.fetch_add(1, std::memory_order_relaxed);
        else
          Output.keep();
      }
    });
  }

  while (Ready.load(std::memory_order_acquire) != ThreadCount)
    std::this_thread::yield();
  Start.store(true, std::memory_order_release);
  for (std::thread &Thread : Threads)
    Thread.join();

  EXPECT_EQ(Failures.load(std::memory_order_relaxed), 0u);
}
#endif

} // namespace
