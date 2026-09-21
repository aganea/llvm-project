//===-- llvm-driver.cpp ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/HTTP/HTTPClient.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/Driver.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define LLVM_DRIVER_TOOL(tool, entry, per_invocation)                          \
  int entry##_main(int argc, char **argv, const llvm::ToolContext &);
#include "LLVMDriverTools.def"

// Lifecycle entry points exist only for tools built with per-invocation
// globals, so an unconditional declaration would be an undefined reference.
#define LLVM_DRIVER_LIFECYCLE_DECL_0(entry)
#define LLVM_DRIVER_LIFECYCLE_DECL_1(entry) void entry##_init_lifecycle();
#define LLVM_DRIVER_TOOL(tool, entry, per_invocation)                          \
  LLVM_DRIVER_LIFECYCLE_DECL_##per_invocation(entry)
#include "LLVMDriverTools.def"

// Targets and their components are an immutable process-wide catalog. In
// per-invocation mode, populate it before any invocation can overlap. Tool
// entry points may still repeat these calls; TargetRegistry makes identical
// component registrations synchronized no-ops.
static constexpr bool HasPerInvocationGlobals =
#define LLVM_DRIVER_TOOL(tool, entry, per_invocation) per_invocation ||
#include "LLVMDriverTools.def"
    false;

constexpr char subcommands[] =
#define LLVM_DRIVER_TOOL(tool, entry, per_invocation) "  " tool "\n"
#include "LLVMDriverTools.def"
    ;

static void printHelpMessage() {
  llvm::outs() << "OVERVIEW: llvm toolchain driver\n\n"
               << "USAGE: llvm [subcommand] [options]\n\n"
               << "SUBCOMMANDS:\n\n"
               << subcommands
               << "\n  Type \"llvm <subcommand> --help\" to get more help on a "
                  "specific subcommand\n\n"
               << "OPTIONS:\n\n  --help - Display this message\n";
}

int main(int Argc, char **Argv) {
  const CallableTool Tools[] = {
#define LLVM_DRIVER_LIFECYCLE_INIT_0(entry) nullptr
#define LLVM_DRIVER_LIFECYCLE_INIT_1(entry) entry##_init_lifecycle
#define LLVM_DRIVER_TOOL(tool, entry, per_invocation)                          \
  {tool, [](int Argc, char **Argv, const ToolContext &Context) {               \
     return runLLVMDriverTool(                                                 \
         LLVM_DRIVER_LIFECYCLE_INIT_##per_invocation(entry), entry##_main,     \
         Argc, Argv, Context);                                                 \
   }},
#include "LLVMDriverTools.def"
  };

  ToolSession Session(Argc, Argv, Tools);

  StringRef Stem = sys::path::stem(Argv[0]);
  if (Stem.equals_insensitive("llvm") &&
      (Argc == 1 || (Argc == 2 && StringRef(Argv[1]) == "--help"))) {
    printHelpMessage();
    return Argc == 1 ? 1 : 0;
  }

  auto RunTool = [&]() {
    SmallVector<const char *, 16> Args(Argv, Argv + Argc);
    ErrorOr<int> Result = Session.callTool(Args);
    if (!Result) {
      printHelpMessage();
      return 1;
    }
    return *Result;
  };

  if constexpr (HasPerInvocationGlobals) {
    // HTTP is a process-wide runtime service.  Initialize it once on the
    // startup thread so folded tools may use independent clients concurrently
    // without racing their otherwise-idempotent initialize() calls.
    llvm::HTTPClient::initialize();
    llvm::scope_exit HTTPClientCleanup([] { llvm::HTTPClient::cleanup(); });

    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllDisassemblers();
    // LTO backends still construct legacy IR and CodeGen passes. Register the
    // same production pass set as llc before a nested linker invocation can
    // start worker threads; lazy registration from one of those workers would
    // mutate the process-wide registry while invocation contexts are active.
    llvm::PassRegistry &Registry = *llvm::PassRegistry::getPassRegistry();
    llvm::initializeCore(Registry);
    llvm::initializeCodeGen(Registry);
    llvm::initializeLoopStrengthReducePass(Registry);
    llvm::initializePostInlineEntryExitInstrumenterPass(Registry);
    llvm::initializeUnreachableBlockElimLegacyPassPass(Registry);
    llvm::initializeConstantHoistingLegacyPassPass(Registry);
    llvm::initializeScalarOpts(Registry);
    llvm::initializeIPO(Registry);
    llvm::initializeVectorization(Registry);
    llvm::initializeScalarizeMaskedMemIntrinLegacyPassPass(Registry);
    llvm::initializeTransformUtils(Registry);
    return RunTool();
  }
  return RunTool();
}
