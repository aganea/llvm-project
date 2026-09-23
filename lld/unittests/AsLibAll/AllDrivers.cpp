//===- AllDrivers.cpp -------------------------------------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This test shows a typical case where all LLD drivers are linked into the
// application binary. This is very similar to how lld.exe binary is linked,
// except that here we cleanup the internal LLD memory context after each call.
//===----------------------------------------------------------------------===//

#include "lld/Common/Driver.h"
#include "llvm/Support/Driver.h"
#include "gmock/gmock.h"

LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(mingw)
LLD_HAS_DRIVER(macho)
LLD_HAS_DRIVER(wasm)

int lld_main(int argc, char **argv, const llvm::ToolContext &);

static bool lldInvoke(std::vector<const char *> args) {
  args.push_back("--version");
  lld::Result r =
      lld::lldMain(args, llvm::outs(), llvm::errs(), LLD_ALL_DRIVERS);
  return !r.retCode && r.canRunAgain;
}

TEST(AsLib, AllDrivers) {
  EXPECT_TRUE(lldInvoke({"ld.lld"}));
  EXPECT_TRUE(lldInvoke({"ld64.lld"}));
  EXPECT_TRUE(lldInvoke({"ld", "-m", "i386pe"})); // MinGW
  EXPECT_TRUE(lldInvoke({"lld-link"}));
  EXPECT_TRUE(lldInvoke({"wasm-ld"}));
}

#if defined(_WIN32) || defined(__ELF__) || defined(__wasm__)
static void noOpLifecycle() {}

static int invokeScopedLld() {
  char Arg0[] = "ld.lld";
  char Arg1[] = "--version";
  char *Args[] = {Arg0, Arg1};
  llvm::ScopedToolInvocation Invocation(noOpLifecycle);
  llvm::ToolContext Context{Arg0, nullptr, false};
  return lld_main(std::size(Args), Args, Context);
}
#endif

TEST(AsLib, ScopedInvocationReturnsAndCanRunAgain) {
#if !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  GTEST_SKIP() << "the v1 producer supports COFF, ELF, and Wasm only";
#else
  EXPECT_EQ(0, invokeScopedLld());
  EXPECT_EQ(0, invokeScopedLld());
#endif
}
