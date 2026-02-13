//===-- DriverUtils.cpp ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Utilities for the LLVM multi-call driver.
//
// This file provides the infrastructure for two related use cases:
//
// 1. The "multicall" binary (llvm.exe / llvm-driver): a single executable that
//    bundles many LLVM tools. The tool is selected at runtime based on argv[0]
//    or an explicit subcommand: `llvm clang++ -O2 foo.cpp`.
//
// 2. Individual standalone tool binaries (e.g. clang.exe, lld-link.exe) that
//    can also call other tools in-process via ToolContext::callTool().
//
// Both paths converge on a shared dispatch function (dispatchDriverTools) that
// registers tools into a ProcessState and dispatches to the matching one.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/LLVMDriver.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

//===----------------------------------------------------------------------===//
// ProcessState -- per-process tool registry
//===----------------------------------------------------------------------===//

/// Holds the set of tools that have been registered in this process, along with
/// a set of module names that have already been attempted for dynamic loading.
///
/// Each entry point (LLVMDriverMain for the multicall binary, or
/// LLVMDriverCallMain for standalone binaries) creates a local ProcessState on
/// the stack, populates it with the initially known tools, and passes it to a
/// ToolContext via buildRoot(). Additional tools may be added lazily by
/// getCallableTool() when it dynamically loads external modules (Windows only).
struct ProcessState {
  /// All tools currently known to this process. Populated at startup by
  /// dispatchDriverTools() and potentially extended at runtime by
  /// getCallableTool() after loading external modules.
  std::vector<CallableTool> RegisteredTools;

  /// Register a tool, deduplicating by name (case-insensitive). If a tool with
  /// the same name is already registered, this is a no-op.
  void RegisterDriverTool(CallableTool Tool) {
    auto It = llvm::find_if(RegisteredTools, [&](CallableTool &T) {
      return T.ToolName.equals_insensitive(Tool.ToolName);
    });
    if (It == RegisteredTools.end())
      RegisteredTools.emplace_back(Tool);
  }
};

//===----------------------------------------------------------------------===//
// Tool name matching and argument adjustment
//===----------------------------------------------------------------------===//

/// Check whether \p Argv0 (a binary path or stem) refers to \p RegisteredTool.
///
/// This uses a fuzzy match: the registered tool name must appear as a suffix of
/// the stem or filename (delimited by a non-alphanumeric character), or match
/// after replacing hyphens with underscores. For example, "llvm-ar" matches
/// "ar", and "x86_64-unknown-linux-gnu-ar" also matches "ar".
static bool Is(StringRef RegisteredTool, StringRef Argv0) {
  auto IsImpl = [=](StringRef Stem) {
    auto I = Stem.rfind_insensitive(RegisteredTool);
    return I != StringRef::npos &&
           (I + RegisteredTool.size() == Stem.size() ||
            !llvm::isAlnum(Stem[I + RegisteredTool.size()]));
  };
  for (StringRef S : {sys::path::stem(Argv0), sys::path::filename(Argv0)})
    if (IsImpl(S))
      return true;
  std::string ToolName = sys::path::stem(Argv0).str();
  if (StringRef(ToolName).equals_insensitive(RegisteredTool))
    return true;
  std::replace(ToolName.begin(), ToolName.end(), '-', '_');
  return StringRef(ToolName).starts_with_insensitive(RegisteredTool) ||
         StringRef(ToolName).ends_with_insensitive(RegisteredTool);
}

/// Returns true if \p Path refers to the multicall binary (i.e. "llvm" or
/// "llvm.exe").
bool ToolContext::isMulticallBinary(StringRef Path) {
  return sys::path::stem(Path).equals_insensitive("llvm");
}

/// Adjust \p Args for multicall invocation.
///
/// If Args[0] is the multicall binary ("llvm"), the subcommand is in Args[1],
/// so we drop the first element so that the tool sees itself as Args[0].
/// Returns the tool name regardless of invocation style.
static StringRef adjustArgs(ArrayRef<const char *> &Args) {
  StringRef ToolName;
  if (Args.size() > 1 && ToolContext::isMulticallBinary(Args[0])) {
    // Multi-call form: `llvm clang++ ...` -> tool is Args[1].
    ToolName = Args[1];
    Args = Args.drop_front();
  } else {
    ToolName = Args[0];
  }
  return ToolName;
}

//===----------------------------------------------------------------------===//
// ToolContext methods
//===----------------------------------------------------------------------===//

/// Build a child ToolContext from the current context and the given arguments.
/// The child inherits the ProcessState and records the binary path (and tool
/// name, if multicall) so that tools can re-invoke themselves.
ToolContext ToolContext::build(ArrayRef<const char *> Args) const {
  ArrayRef<const char *> InitialArgs = Args;
  StringRef ToolName = adjustArgs(Args);

  ToolContext New;
  New.State = State;
  New.BinArgs.push_back(InitialArgs[0]);
  if (InitialArgs != Args)
    New.BinArgs.push_back(ToolName.data());
  return New;
}

/// Return the logical tool name for this context. For multicall binaries this
/// is the subcommand (e.g. "clang++"); for standalone binaries it is the stem
/// of the executable path (e.g. "clang-cl").
StringRef ToolContext::getToolName() const {
  if (ToolContext::isMulticallBinary(BinArgs[0]))
    return BinArgs[1];
  return sys::path::stem(BinArgs[0]);
}

/// Look up the current context's tool using invocationArgs().
ErrorOr<CallableTool> ToolContext::getCallableTool() const {
  ArrayRef<const char *> Args = BinArgs;
  return getCallableTool(Args);
}

/// Look up a tool by examining \p Args. First checks the set of registered
/// (linked-in) tools; if not found, attempts to dynamically load the tool
/// module from the same directory as the current binary (Windows only).
///
/// On success, \p Args is adjusted to remove the multicall prefix (if any),
/// so the returned tool sees itself as Args[0].
ErrorOr<CallableTool>
ToolContext::getCallableTool(ArrayRef<const char *> &Args) const {
  StringRef ToolName = adjustArgs(Args);

  ProcessState &State = *static_cast<ProcessState *>(this->State);

  // Look for registered tools.
  for (auto &RegisteredTool : State.RegisteredTools) {
    if (Is(RegisteredTool.ToolName, ToolName))
      return RegisteredTool;
  }

  return make_error_code(std::errc::no_such_file_or_directory);
}

/// Invoke a registered tool with the appropriate child ToolContext.
/// Adjusts args for multicall invocation before calling the tool's main
/// function.
int CallableTool::call(const ToolContext &Ctx,
                       ArrayRef<const char *> Args) const {
  ToolContext NewCtx = Ctx.build(Args);
  // Adjust the args in case we are dealing with a multicall command-line,
  // to only keep the tool name as the first arg.
  adjustArgs(Args);
  return MainFn(Args, NewCtx);
}

/// Look up and call the tool identified by \p Args[0] (or Args[1] for
/// multicall). Returns -1 if the tool is not found.
int ToolContext::callTool(ArrayRef<const char *> Args) const {
  auto EC = getCallableTool(Args);
  if (EC)
    return EC.get().call(*this, Args);
  return -1;
}

/// Create a root ToolContext from an externally managed ProcessState.
/// The caller is responsible for keeping \p State alive for the lifetime of
/// the returned ToolContext and any children derived from it.
ToolContext ToolContext::buildRoot(void *State) {
  ToolContext Root;
  Root.State = State;
  return Root;
}

//===----------------------------------------------------------------------===//
// Entry points (not exposed in LLVMDriver.h -- callers forward-declare them)
//===----------------------------------------------------------------------===//

/// Print the help message for the multicall driver, listing all available
/// subcommands from the given tool array.
static void printDriverHelp(ArrayRef<CallableTool> Tools) {
  outs() << "OVERVIEW: llvm toolchain driver\n\n"
         << "USAGE: llvm [subcommand] [options]\n\n"
         << "SUBCOMMANDS:\n\n";
  for (const auto &T : Tools)
    outs() << "  " << T.ToolName << "\n";
  outs() << "\n  Type \"llvm <subcommand> --help\" to get more help on a "
            "specific subcommand\n\n"
         << "OPTIONS:\n\n  --help - Display this message\n";
}

/// Common dispatch logic shared by LLVMDriverMain() and LLVMDriverCallMain().
///
/// Registers all tools from \p Tools into a ProcessState, creates a root
/// ToolContext, and dispatches to the tool identified by \p Args[0].
static int dispatchDriverTools(ArrayRef<const char *> Args,
                               ArrayRef<CallableTool> Tools) {
  ProcessState State;
  for (const auto &T : Tools)
    State.RegisterDriverTool(T);

  auto Ctx = ToolContext::buildRoot(&State);
  return Ctx.callTool(Args);
}

/// Entry point for the llvm multicall driver (called from llvm-driver.cpp).
///
/// Handles --help when invoked as the multicall binary (llvm.exe), then
/// dispatches to the requested subcommand. When invoked via a tool-named
/// hardlink (e.g. clang.exe), skips the driver help and dispatches directly
/// so that the tool handles its own --help/no-args behavior.
/// The \p Tools array is built by llvm-driver.cpp from LLVMDriverTools.def.
int LLVMDriverMain(int Argc, char **Argv, ArrayRef<CallableTool> Tools) {
  InitLLVM X(Argc, Argv);
  ArrayRef<const char *> Args = X.getArgs();

  // Only show the driver help when explicitly invoked as the multicall binary
  // (i.e. "llvm.exe" or "llvm.exe --help"). When invoked via a tool-named
  // hardlink (e.g. "clang.exe --help"), let the tool handle it.
  if (ToolContext::isMulticallBinary(Args[0])) {
    if (Args.size() <= 1 ||
        (Args.size() > 1 && StringRef(Args[1]) == "--help")) {
      printDriverHelp(Tools);
      return Args.size() <= 1 ? 1 : 0;
    }
  }

  return dispatchDriverTools(Args, Tools);
}

/// Entry point for individual standalone tool binaries (called from
/// LLVM_DRIVER_IMPL_MAIN-generated main() functions).
///
/// Registers the tool under both its canonical name (e.g. "lld") and the
/// binary name from argv[0] (e.g. "lld-link"), then dispatches.
int LLVMDriverCallMain(int Argc, char **Argv, StringRef Name,
                       ToolMainFn MainFn) {
  InitLLVM X(Argc, Argv);
  StringRef ThisTool = sys::path::stem(X.getArgs()[0]);

  const CallableTool Tools[] = {
      {Name, MainFn},     // Canonical name (e.g. "lld")
      {ThisTool, MainFn}, // Binary stem name (e.g. "lld-link")
  };

  return dispatchDriverTools(X.getArgs(), Tools);
}
