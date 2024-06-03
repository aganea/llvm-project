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

// Absolute Argv[0] set when the PE/binary starts.
static const char *InitialArgv0;

// A list of LLVM tools that live inside the current PE/binary. If the binary
// embeds a single tool that isn't known to llvm-driver, this can empty or
// nullptr.
static ToolContext::KnownToolsFn KnownTools;

void ToolContext::setTools(KnownToolsFn Tools, const char *Argv0) {
  KnownTools = Tools;
  InitialArgv0 = Argv0;
}

StringRef ToolContext::getArgv0() const {
  return InitialArgv0 ? InitialArgv0 : BinaryPath;
}

static std::optional<ToolContext> discoverTool(const char *Arg0,
                                               const char *Arg1) {
  if (!KnownTools)
    return ToolContext{Arg0};

  // Clean tool name
  StringRef Tool = sys::path::filename(Arg0);
  if (Tool.ends_with_insensitive(".exe"))
    Tool = Tool.drop_back(4);

  StringRef BestTool;
  ToolContext::MainFn BestToolMain{};

  auto MainFns = KnownTools();
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
  if (!BestToolMain) {
    if (!Arg1)
      return std::nullopt;

    auto TC = discoverTool(Arg1, nullptr);
    if (!TC)
      return std::nullopt;

    TC->BinaryPath = Arg0;
    TC->ProvidedToolName = Arg1;
    return TC;
  }

  ToolContext TC{Arg0};
  TC.Main = BestToolMain;
  TC.VerbatimToolName = BestTool;
  return TC;
}

static std::optional<ToolContext>
discoverToolArgs(ArrayRef<const char *> Args) {
  if (Args.size() >= 2) {
    return discoverTool(Args[0], Args[1]);
  } else if (Args.size() == 1) {
    return discoverTool(Args[0], nullptr);
  }
  return std::nullopt;
}

int ToolContext::callToolMain(ArrayRef<const char *> Args) const {
  if (!Main)
    return -1; // as per `llvm::sys::ExecuteAndWait()`.
  return Main(Args.size(), const_cast<char **>(Args.data()), *this);
}

std::optional<ToolContext>
ToolContext::newContext(ArrayRef<const char *> Args) const {
  auto TC = discoverToolArgs(Args);
  if (!TC)
    return std::nullopt;

  // We're asking for the same tool, just return this one.
  // This avoids returning `llvm.exe clang` when we could just return
  // `clang-cl.exe` as it was called on the command-line.
  if (!VerbatimToolName.empty() && TC->VerbatimToolName == VerbatimToolName)
    return *this;

  TC->Cleanup = Cleanup;
  return TC;
}

StringRef ToolContext::getProgramName() const {
  return !ProvidedToolName.empty() ? ProvidedToolName
                                   : sys::path::filename(BinaryPath);
}

StringRef GetExecutablePath(bool CanonicalPrefixes) {
  if (!CanonicalPrefixes) {
    static SmallString<128> ExecutablePath(InitialArgv0);
    // Do a PATH lookup if Argv0 isn't a valid path.
    if (!sys::fs::exists(ExecutablePath))
      if (ErrorOr<std::string> P = llvm::sys::findProgramByName(ExecutablePath))
        ExecutablePath = *P;
    return ExecutablePath;
  }
  static std::string MainExe =
      sys::fs::getMainExecutable(InitialArgv0, KnownTools);
  return MainExe;
}

void ToolContext::setCanonicalPrefixes(bool CanonicalPrefixes) {
  StringRef Tool = sys::path::filename(BinaryPath);
  BinaryPath = GetExecutablePath(CanonicalPrefixes).data();

  // Retain the name of the tool as it was invoked, if ever we resolved to a new
  // binary name. This can happen if the initial invocation was a symlink.
  if (sys::path::filename(BinaryPath) != Tool && ProvidedToolName.empty()) {
    if (Tool.ends_with_insensitive(".exe"))
      Tool = Tool.drop_back(4);
    ProvidedToolName = Tool;
  }
}

std::vector<const char *> ToolContext::executionArgs() const {
  return ProvidedToolName.empty()
             ? std::vector{BinaryPath.data()}
             : std::vector{BinaryPath.data(), ProvidedToolName.data()};
}

// The stringified arguments that would be used to re-invoke this tool.
std::string ToolContext::executionArgsString() const {
  std::string S;
  raw_string_ostream OS(S);
  llvm::sys::printArg(OS, BinaryPath, /*Quote=*/true);
  if (!ProvidedToolName.empty()) {
    OS << ' ';
    llvm::sys::printArg(OS, ProvidedToolName, /*Quote=*/true);
  }
  return S;
}
