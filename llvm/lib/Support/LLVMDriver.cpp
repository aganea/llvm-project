//===- LLVMDriver.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/LLVMDriver.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <vector>

using namespace llvm;

static StringRef cleanToolName(StringRef Argv0) {
  StringRef ToolName = sys::path::filename(Argv0);
  if (ToolName.ends_with_insensitive(".exe"))
    ToolName = ToolName.drop_back(4);
  return ToolName;
}

static std::pair<StringRef, ToolContext::MainFn>
discoverTool(StringRef Tool, ToolContext::KnownToolsFn KnownTools) {
  if (!KnownTools)
    return std::make_pair<StringRef, ToolContext::MainFn>({}, nullptr);

  StringRef BestTool;
  ToolContext::MainFn BestToolMain{};

  auto MainFns = KnownTools();
  llvm::for_each(MainFns, [&](auto &Pair) {
    StringRef VerbatimTool(Pair.first);
    auto I = Tool.rfind_insensitive(VerbatimTool);
    if (I == StringRef::npos)
      return;
    if (VerbatimTool.size() <= BestTool.size())
      return;
    if (I > 0 && llvm::isAlnum(Tool[I - 1]))
      return;
    if (I + VerbatimTool.size() < Tool.size() &&
        llvm::isAlnum(Tool[I + VerbatimTool.size()]))
      return;
    BestTool = VerbatimTool;
    BestToolMain = Pair.second;
  });
  return std::make_pair<StringRef, ToolContext::MainFn>(
      std::move(BestTool), std::move(BestToolMain));
}

bool llvm::ToolContext::hasInProcessTool(ArrayRef<const char *> Args) const {
  StringRef Tool = cleanToolName(Args[0]);
  if (Tool.equals_insensitive("llvm"))
    return hasInProcessTool(Args.drop_front(1));

  auto [BestTool, BestToolMain] = discoverTool(Tool, KnownTools);
  return !!BestToolMain;
}

int llvm::ToolContext::callToolMain(ArrayRef<const char *> Args) const {
  StringRef Tool = cleanToolName(Args[0]);
  if (Tool.equals_insensitive("llvm"))
    return callToolMain(Args.drop_front(1));

  auto [BestTool, BestToolMain] = discoverTool(Tool, KnownTools);
  if (!BestToolMain)
    return -1; // as per `llvm::sys::ExecuteAndWait()`.

  bool NeedsPrependArg = Args[0] != Path;
  ToolContext NewTC{NeedsPrependArg ? Path : Args[0], BestTool.data(),
                    NeedsPrependArg, /*Cleanup=*/Cleanup, KnownTools};
  return BestToolMain(Args.size(), const_cast<char **>(Args.data()), NewTC);
}

std::optional<ToolContext> llvm::ToolContext::newContext(StringRef Tool) {
  // This just needs to be some symbol in the binary
  static int Symbol;
  static std::string MainBinary =
      llvm::sys::fs::getMainExecutable(Path, &Symbol);

  Tool = cleanToolName(Tool);
  auto [BestTool, BestToolMain] = discoverTool(Tool, KnownTools);
  if (!BestToolMain)
    return std::nullopt;

  ToolContext NewTC{MainBinary.c_str(), BestTool.data(),
                    /*NeedsPrependArg=*/true, Cleanup, KnownTools};
  return NewTC;
}
