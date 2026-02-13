//===- ToolContextTest.cpp - Tests for ToolContext and CallableTool
//--------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Unit tests for the public ToolContext and CallableTool API surface.
//
// NOTE: These tests deliberately avoid exercising callTool() and
// getCallableTool(), which depend on the internal ProcessState struct defined
// in DriverUtils.cpp. The Is() fuzzy matching and dispatch logic are covered
// by the llvm-driver lit tests (llvm/test/tools/llvm-driver/) which exercise
// the real binary.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/LLVMDriver.h"
#include "gtest/gtest.h"

using namespace llvm;

//===----------------------------------------------------------------------===//
// Test helpers
//===----------------------------------------------------------------------===//

// A dummy tool main function that returns the number of args it received.
static int dummyToolMain(ArrayRef<const char *> Args, const ToolContext &) {
  return static_cast<int>(Args.size());
}

//===----------------------------------------------------------------------===//
// isMulticallBinary
//===----------------------------------------------------------------------===//

TEST(ToolContextTest, IsMulticallBinaryPositive) {
  EXPECT_TRUE(ToolContext::isMulticallBinary("llvm"));
  EXPECT_TRUE(ToolContext::isMulticallBinary("llvm.exe"));
  EXPECT_TRUE(ToolContext::isMulticallBinary("LLVM.EXE"));
  EXPECT_TRUE(ToolContext::isMulticallBinary("/usr/bin/llvm"));
  EXPECT_TRUE(ToolContext::isMulticallBinary("C:\\bin\\llvm.exe"));
}

TEST(ToolContextTest, IsMulticallBinaryNegative) {
  EXPECT_FALSE(ToolContext::isMulticallBinary("clang"));
  EXPECT_FALSE(ToolContext::isMulticallBinary("llvm-ar"));
  EXPECT_FALSE(ToolContext::isMulticallBinary("llvm-ar.exe"));
  EXPECT_FALSE(ToolContext::isMulticallBinary("/usr/bin/clang"));
  EXPECT_FALSE(ToolContext::isMulticallBinary(""));
}

//===----------------------------------------------------------------------===//
// ToolContext::build -- standalone path
//===----------------------------------------------------------------------===//

TEST(ToolContextTest, BuildStandalone) {
  // build() only copies the State pointer; it never dereferences it.
  ToolContext Root = ToolContext::buildRoot(nullptr);
  const char *Args[] = {"/usr/bin/clang", "-O2", "foo.c"};
  ToolContext Child = Root.build(Args);

  EXPECT_EQ(Child.getPath(), "/usr/bin/clang");
  EXPECT_EQ(Child.invocationArgs().size(), 1u);
  EXPECT_EQ(StringRef(Child.invocationArgs()[0]), "/usr/bin/clang");
}

//===----------------------------------------------------------------------===//
// ToolContext::build -- multicall path
//===----------------------------------------------------------------------===//

TEST(ToolContextTest, BuildMulticall) {
  ToolContext Root = ToolContext::buildRoot(nullptr);
  const char *Args[] = {"llvm", "clang++", "-O2", "foo.c"};
  ToolContext Child = Root.build(Args);

  EXPECT_EQ(Child.getPath(), "llvm");
  EXPECT_EQ(Child.invocationArgs().size(), 2u);
  EXPECT_EQ(StringRef(Child.invocationArgs()[0]), "llvm");
  EXPECT_EQ(StringRef(Child.invocationArgs()[1]), "clang++");
}

//===----------------------------------------------------------------------===//
// ToolContext::getToolName
//===----------------------------------------------------------------------===//

TEST(ToolContextTest, GetToolNameStandalone) {
  ToolContext Root = ToolContext::buildRoot(nullptr);
  const char *Args[] = {"/usr/bin/clang-cl"};
  ToolContext Child = Root.build(Args);

  EXPECT_EQ(Child.getToolName(), "clang-cl");
}

TEST(ToolContextTest, GetToolNameMulticall) {
  ToolContext Root = ToolContext::buildRoot(nullptr);
  const char *Args[] = {"C:\\bin\\llvm.exe", "lld-link"};
  ToolContext Child = Root.build(Args);

  EXPECT_EQ(Child.getToolName(), "lld-link");
}

//===----------------------------------------------------------------------===//
// CallableTool bool conversion
//===----------------------------------------------------------------------===//

TEST(ToolContextTest, CallableToolBoolConversion) {
  CallableTool Empty;
  EXPECT_FALSE(Empty);

  CallableTool Valid{"test", dummyToolMain};
  EXPECT_TRUE(Valid);
}

//===----------------------------------------------------------------------===//
// CallableTool::call -- standalone and multicall
//===----------------------------------------------------------------------===//
//
// call() invokes build() (safe with nullptr state) and adjustArgs() (a static
// function that doesn't touch state), then calls MainFn. This path never
// dereferences the ProcessState pointer.

TEST(ToolContextTest, CallToolStandalone) {
  CallableTool Tool{"echo", dummyToolMain};
  ToolContext Root = ToolContext::buildRoot(nullptr);
  const char *Args[] = {"/usr/bin/echo", "hello", "world"};

  // dummyToolMain returns Args.size(). For non-multicall, Args stays as-is.
  int Result = Tool.call(Root, Args);
  EXPECT_EQ(Result, 3);
}

TEST(ToolContextTest, CallToolMulticall) {
  CallableTool Tool{"echo", dummyToolMain};
  ToolContext Root = ToolContext::buildRoot(nullptr);
  const char *Args[] = {"llvm", "echo", "hello"};

  // After adjustArgs for multicall, Args becomes {"echo", "hello"} (2
  // elements).
  int Result = Tool.call(Root, Args);
  EXPECT_EQ(Result, 2);
}
