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
#include "llvm/Support/Program.h"
#include <vector>

using namespace llvm;

ToolContext::KnownToolsFn ToolContext::KnownTools;
const char *ToolContext::Argv0;

static std::pair<StringRef, ToolContext::MainFn>
discoverTool(ArrayRef<const char *> &Args) {
  assert(ToolContext::KnownTools);
  if (!Args.size())
    return std::make_pair<StringRef, ToolContext::MainFn>({}, nullptr);

  // Clean tool name
  StringRef Tool = sys::path::filename(Args[0]);
  if (Tool.ends_with_insensitive(".exe"))
    Tool = Tool.drop_back(4);
  if (Tool.equals_insensitive("llvm")) {
    Args = Args.drop_front(1);
    return discoverTool(Args);
  }

  StringRef BestTool;
  ToolContext::MainFn BestToolMain{};

  auto MainFns = ToolContext::KnownTools();
  for_each(MainFns, [&](auto &Pair) {
    StringRef VerbatimTool(Pair.first);
    auto I = Tool.rfind_insensitive(VerbatimTool);
    if (I == StringRef::npos)
      return;
    if (VerbatimTool.size() <= BestTool.size())
      return;
    if (I > 0 && isAlnum(Tool[I - 1]))
      return;
    if (I + VerbatimTool.size() < Tool.size() &&
        isAlnum(Tool[I + VerbatimTool.size()]))
      return;
    BestTool = VerbatimTool;
    BestToolMain = Pair.second;
  });
  return std::make_pair<StringRef, ToolContext::MainFn>(
      std::move(BestTool), std::move(BestToolMain));
}

bool ToolContext::hasInProcessTool(ArrayRef<const char *> Args) const {
  auto [BestTool, BestToolMain] = discoverTool(Args);
  return !!BestToolMain;
}

int ToolContext::callToolMain(ArrayRef<const char *> Args) const {
  auto [BestTool, BestToolMain] = discoverTool(Args);
  if (!BestToolMain)
    return -1; // as per `llvm::sys::ExecuteAndWait()`.

  bool NeedsPrependArg = Args[0] != Path;
  ToolContext NewTC{NeedsPrependArg ? Path : Args[0], BestTool.data(),
                    NeedsPrependArg, /*Cleanup=*/Cleanup};
  return BestToolMain(Args.size(), const_cast<char **>(Args.data()), NewTC);
}

std::optional<ToolContext>
ToolContext::newContext(ArrayRef<const char *> Args) const {
  static std::string MainBinary =
      sys::fs::getMainExecutable(Path, MainSymbol);

  auto [BestTool, BestToolMain] = discoverTool(Args);
  if (!BestToolMain)
    return std::nullopt;

  ToolContext NewTC{MainBinary.c_str(), BestTool.data(),
                    /*NeedsPrependArg=*/true, Cleanup};
  return NewTC;
}

StringRef GetExecutablePath(bool CanonicalPrefixes) {
  if (!CanonicalPrefixes) {
    SmallString<128> ExecutablePath(ToolContext::Argv0);
    // Do a PATH lookup if Argv0 isn't a valid path.
    if (!sys::fs::exists(ExecutablePath))
      if (ErrorOr<std::string> P = llvm::sys::findProgramByName(ExecutablePath))
        ExecutablePath = *P;
    return ExecutablePath;
  }
  static std::string MainExe =
      sys::fs::getMainExecutable(ToolContext::Argv0, ToolContext::KnownTools);
  return MainExe;
}

void ToolContext::setCanonicalPrefixes(bool CanonicalPrefixes) {
  Path = GetExecutablePath(CanonicalPrefixes).data();
}
