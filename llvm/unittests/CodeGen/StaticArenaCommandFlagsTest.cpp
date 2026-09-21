//===- StaticArenaCommandFlagsTest.cpp - Real CommandFlags arena test -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/StaticArena.h"
#include "llvm/Support/ThreadPool.h"
#include "gtest/gtest.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

using namespace llvm;

namespace {

struct InvocationResult {
  bool Parsed = false;
  std::string OwnerCPU;
  std::string WorkerCPU;
};

TEST(StaticArenaCommandFlagsTest, ConcurrentMCPUUsesInvocationStorage) {
#if !defined(_WIN32) && !defined(__ELF__)
  GTEST_SKIP() << "the v1 producer supports COFF and ELF only";
#else
  DefaultThreadPool Pool(hardware_concurrency(2));
  std::atomic<unsigned> Ready{0};
  std::atomic<bool> RunWorkers{false};
  InvocationResult A;
  InvocationResult B;

  auto RunInvocation = [&](StringRef CPU, InvocationResult &Result) {
    std::unique_ptr<StaticArena> Arena = StaticArena::create();
    {
      ScopedStaticArenaBinding ArenaBinding(*Arena);
      {
        cl::ScopedContext CommandLineContext;
        codegen::RegisterCodeGenFlags Flags;

        std::string CPUArgument = ("-mcpu=" + CPU).str();
        const char *Arguments[] = {"static-arena-command-flags-test",
                                   CPUArgument.c_str()};
        Result.Parsed = cl::ParseCommandLineOptions(2, Arguments);
        Result.OwnerCPU = codegen::getMCPU();

        Ready.fetch_add(1, std::memory_order_release);
        while (!RunWorkers.load(std::memory_order_acquire))
          std::this_thread::yield();

        std::shared_future<std::string> Future =
            Pool.async([] { return codegen::getMCPU(); });
        Result.WorkerCPU = Future.get();

        // Match llvm-driver's root lifecycle: stop command-line captures
        // first, finalize arena objects while that context remains current,
        // then let the context and owner binding unwind before storage release.
        CommandLineContext.beginClosing();
        Arena->beginClosing();
        Arena->runDestructors();
      }
    }
  };

  std::thread ThreadA(RunInvocation, "znver2", std::ref(A));
  std::thread ThreadB(RunInvocation, "skylake", std::ref(B));
  while (Ready.load(std::memory_order_acquire) != 2)
    std::this_thread::yield();
  RunWorkers.store(true, std::memory_order_release);
  ThreadA.join();
  ThreadB.join();
  Pool.wait();

  EXPECT_TRUE(A.Parsed);
  EXPECT_TRUE(B.Parsed);
  EXPECT_EQ("znver2", A.OwnerCPU);
  EXPECT_EQ("znver2", A.WorkerCPU);
  EXPECT_EQ("skylake", B.OwnerCPU);
  EXPECT_EQ("skylake", B.WorkerCPU);
#endif
}

} // namespace
