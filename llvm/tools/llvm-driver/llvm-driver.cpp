//===-- llvm-driver.cpp ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/HTTP/HTTPClient.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/Driver.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ProcessWideRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/ToolExecutionContext.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define LLVM_DRIVER_TOOL(tool, entry, per_invocation, process_initialization)  \
  int entry##_main(int argc, char **argv, const llvm::ToolContext &);
#include "LLVMDriverTools.def"

// Lifecycle entry points exist only for tools built with per-invocation
// globals, so an unconditional declaration would be an undefined reference.
#define LLVM_DRIVER_LIFECYCLE_DECL_0(entry)
#define LLVM_DRIVER_LIFECYCLE_DECL_1(entry) void entry##_init_lifecycle();
#define LLVM_DRIVER_TOOL(tool, entry, per_invocation, process_initialization)  \
  LLVM_DRIVER_LIFECYCLE_DECL_##per_invocation(entry)
#include "LLVMDriverTools.def"

#if LLVM_DRIVER_HAS_PROCESS_INITIALIZATION
static llvm::once_flag LLVMDriverProcessInitializationFlag;

static void initializeLLVMDriverProcessState() {
  llvm::call_once(LLVMDriverProcessInitializationFlag, [] {
    // A catalog-using tool may be called synchronously by an already-running
    // lightweight tool. Process initialization must not borrow that outer
    // invocation's arena or command-line context. Mask both bindings so any
    // accidental arena dependency fails closed and command-line registration
    // remains process-owned.
    llvm::ScopedToolExecutionContext NeutralContext{
        llvm::ToolExecutionContext()};

    // The generated dispatch classification keeps target and CodeGen catalog
    // consumers behind this call_once. Other tools may already be running, so
    // permit only this bootstrap thread to perform the audited late writes.
    llvm::ProcessWideRegistryBootstrap Bootstrap;

    // Targets and their components are an immutable process-wide catalog.
    // Populate them before the first target-using invocation installs its
    // context. Registration is internally synchronized, so this first use may
    // overlap a lightweight tool whose finalized closure cannot consume the
    // target or CodeGen catalogs.
#if LLVM_DRIVER_HAS_TARGETS
    llvm::InitializeAllTargets();
#elif LLVM_DRIVER_HAS_TARGET_INFOS
    llvm::InitializeAllTargetInfos();
#endif
#if LLVM_DRIVER_HAS_TARGET_MCS
    llvm::InitializeAllTargetMCs();
#endif
#if LLVM_DRIVER_HAS_TARGET_MCAS
    llvm::InitializeAllTargetMCAs();
#endif
#if LLVM_DRIVER_HAS_ASM_PRINTERS
    llvm::InitializeAllAsmPrinters();
#endif
#if LLVM_DRIVER_HAS_ASM_PARSERS
    llvm::InitializeAllAsmParsers();
#endif
#if LLVM_DRIVER_HAS_DISASSEMBLERS
    llvm::InitializeAllDisassemblers();
#endif

#if LLVM_DRIVER_HAS_CORE_PASSES || LLVM_DRIVER_HAS_CODEGEN_PASSES ||           \
    LLVM_DRIVER_HAS_SCALAR_PASSES || LLVM_DRIVER_HAS_IPO_PASSES ||             \
    LLVM_DRIVER_HAS_VECTORIZE_PASSES || LLVM_DRIVER_HAS_TRANSFORM_UTILS_PASSES
    // LTO backends still construct legacy IR and CodeGen passes. Register the
    // linked subset of llc's production pass set before a nested linker
    // invocation can start worker threads.
    llvm::PassRegistry &Registry = *llvm::PassRegistry::getPassRegistry();
#endif
#if LLVM_DRIVER_HAS_CORE_PASSES
    llvm::initializeCore(Registry);
#endif
#if LLVM_DRIVER_HAS_CODEGEN_PASSES
    llvm::initializeCodeGen(Registry);
#endif
#if LLVM_DRIVER_HAS_SCALAR_PASSES
    llvm::initializeScalarOpts(Registry);
#endif
#if LLVM_DRIVER_HAS_IPO_PASSES
    llvm::initializeIPO(Registry);
#endif
#if LLVM_DRIVER_HAS_VECTORIZE_PASSES
    llvm::initializeVectorization(Registry);
#endif
#if LLVM_DRIVER_HAS_TRANSFORM_UTILS_PASSES
    llvm::initializeTransformUtils(Registry);
#endif
  });
}

#define LLVM_DRIVER_PROCESS_INITIALIZATION(requires_initialization)            \
  do {                                                                         \
    if constexpr (requires_initialization)                                     \
      initializeLLVMDriverProcessState();                                      \
  } while (false)
#else
#define LLVM_DRIVER_PROCESS_INITIALIZATION(requires_initialization)
#endif

constexpr char subcommands[] =
#define LLVM_DRIVER_TOOL(tool, entry, per_invocation, process_initialization)  \
  "  " tool "\n"
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
#define LLVM_DRIVER_TOOL(tool, entry, per_invocation, process_initialization)  \
  {tool, [](int Argc, char **Argv, const ToolContext &Context) {               \
     LLVM_DRIVER_PROCESS_INITIALIZATION(process_initialization);               \
     return runLLVMDriverTool(                                                 \
         LLVM_DRIVER_LIFECYCLE_INIT_##per_invocation(entry), entry##_main,     \
         Argc, Argv, Context);                                                 \
   }},
#include "LLVMDriverTools.def"
  };

  // curl_global_init was not guaranteed thread-safe before libcurl 7.84.
  // Preserve lazy initialization on capable libcurl and WinHTTP builds, but
  // initialize older libcurl while llvm-driver is still single-threaded.
  if (HTTPClient::isAvailable() && !HTTPClient::isInitializationThreadSafe())
    HTTPClient::initialize();

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

  return RunTool();
}
