//===- per-invocation.cpp - Folded tool lifecycle smoke test --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/Driver.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/ProcessWideRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/ToolExecutionContext.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

int llvm_ar_main(int Argc, char **Argv, const llvm::ToolContext &);
void llvm_ar_init_lifecycle();
int lld_main(int Argc, char **Argv, const llvm::ToolContext &);
void lld_init_lifecycle();
#ifdef LLVM_DRIVER_TEST_CLANG_TRIPLE
int clang_main(int Argc, char **Argv, const llvm::ToolContext &);
void clang_init_lifecycle();
#endif

namespace {

namespace fs = std::filesystem;

static int invokeTool(void (*InitLifecycle)(), llvm::LLVMDriverToolMain Main,
                      std::vector<std::string> Args) {
  std::vector<char *> Argv;
  Argv.reserve(Args.size());
  for (std::string &Arg : Args)
    Argv.push_back(Arg.data());

  llvm::ToolContext Context{Argv.front(), nullptr, false};
  return llvm::runLLVMDriverTool(InitLifecycle, Main,
                                 static_cast<int>(Argv.size()), Argv.data(),
                                 Context);
}

static int invokeAr(std::vector<std::string> Args) {
  return invokeTool(llvm_ar_init_lifecycle, llvm_ar_main, std::move(Args));
}

static int invokeLld(std::vector<std::string> Args) {
  return invokeTool(lld_init_lifecycle, lld_main, std::move(Args));
}

#ifdef LLVM_DRIVER_TEST_CLANG_TRIPLE
static int invokeClang(std::vector<std::string> Args) {
  return invokeTool(clang_init_lifecycle, clang_main, std::move(Args));
}
#endif

static std::atomic<unsigned> LightweightReady{0};
static std::atomic<bool> ReleaseLightweight{false};
static std::atomic<bool> ProcessInitializationWasNeutral{false};

static void initializeProcessCatalogs();

static int holdLightweightMain(int, char **, const llvm::ToolContext &) {
  LightweightReady.fetch_add(1, std::memory_order_release);
  while (!ReleaseLightweight.load(std::memory_order_acquire))
    std::this_thread::yield();
  // Model a first catalog-using tool dispatched synchronously from this
  // lightweight outer invocation. Both callers race the same process once.
  initializeProcessCatalogs();
  return 0;
}

static void initializeProcessCatalogs() {
  static llvm::once_flag InitializationFlag;
  llvm::call_once(InitializationFlag, [] {
    llvm::ScopedToolExecutionContext NeutralContext{
        llvm::ToolExecutionContext()};
    ProcessInitializationWasNeutral.store(!llvm::hasCurrentStaticArena(),
                                          std::memory_order_release);
    llvm::ProcessWideRegistryBootstrap Bootstrap;
#ifdef LLVM_DRIVER_TEST_CLANG_TRIPLE
    llvm::InitializeAllTargets();
#else
    llvm::InitializeAllTargetInfos();
#endif
    llvm::InitializeAllTargetMCs();
#ifdef LLVM_DRIVER_TEST_CLANG_TRIPLE
    llvm::InitializeAllAsmPrinters();
#endif
    llvm::InitializeAllAsmParsers();

#ifdef LLVM_DRIVER_TEST_CLANG_TRIPLE
    llvm::PassRegistry &Registry = *llvm::PassRegistry::getPassRegistry();
    llvm::initializeCore(Registry);
    llvm::initializeCodeGen(Registry);
    llvm::initializeScalarOpts(Registry);
    llvm::initializeIPO(Registry);
    llvm::initializeVectorization(Registry);
    llvm::initializeTransformUtils(Registry);
#endif
  });
}

static bool testColdProcessInitialization() {
  LightweightReady.store(0, std::memory_order_relaxed);
  ReleaseLightweight.store(false, std::memory_order_relaxed);
  auto HoldLightweight = [] {
    return invokeTool(+[] {}, holdLightweightMain, {"lightweight"});
  };

  int FirstResult = -1;
  int SecondResult = -1;
  std::thread FirstLightweight(
      [&] { FirstResult = HoldLightweight(); });
  std::thread SecondLightweight(
      [&] { SecondResult = HoldLightweight(); });
  while (LightweightReady.load(std::memory_order_acquire) != 2)
    std::this_thread::yield();

  // Let two nested cold target-using dispatches race while their lightweight
  // outer tools own invocation contexts. Exactly one neutral process context
  // populates the catalogs, and the generic late-mutation policy remains
  // strict for every thread outside the scoped bootstrap.
  ReleaseLightweight.store(true, std::memory_order_release);
  FirstLightweight.join();
  SecondLightweight.join();
  return FirstResult == 0 && SecondResult == 0 &&
         ProcessInitializationWasNeutral.load(std::memory_order_acquire);
}

static bool writeFile(const fs::path &Path, const std::string &Contents) {
  std::ofstream OS(Path, std::ios::binary);
  OS << Contents;
  return OS.good();
}

static std::vector<fs::path> createInputs(const fs::path &Directory,
                                          const char *Prefix,
                                          unsigned Count) {
  std::error_code EC;
  fs::create_directories(Directory, EC);
  if (EC)
    return {};

  std::vector<fs::path> Inputs;
  Inputs.reserve(Count);
  for (unsigned I = 0; I != Count; ++I) {
    fs::path Path = Directory / (std::string(Prefix) + std::to_string(I));
    if (!writeFile(Path, Path.filename().string()))
      return {};
    Inputs.push_back(std::move(Path));
  }
  return Inputs;
}

static std::vector<std::string>
archiveArguments(const char *Operation, const fs::path &Archive,
                 const std::vector<fs::path> &Inputs) {
  std::vector<std::string> Args{"llvm-ar", Operation, Archive.string()};
  Args.reserve(Args.size() + Inputs.size());
  for (const fs::path &Input : Inputs)
    Args.push_back(Input.string());
  return Args;
}

static bool hasMagic(const fs::path &Archive, const char (&Magic)[9]) {
  char Actual[8];
  std::ifstream IS(Archive, std::ios::binary);
  IS.read(Actual, sizeof(Actual));
  return IS.gcount() == sizeof(Actual) &&
         std::string(Actual, sizeof(Actual)) == std::string(Magic, 8);
}

static bool extractAndCheck(const fs::path &Archive,
                            const fs::path &OutputDirectory,
                            const std::vector<fs::path> &ExpectedInputs) {
  if (invokeAr({"llvm-ar", "x", Archive.string(), "--output",
                OutputDirectory.string()}) != 0)
    return false;

  std::set<std::string> Expected;
  for (const fs::path &Input : ExpectedInputs)
    Expected.insert(Input.filename().string());

  std::set<std::string> Actual;
  std::error_code EC;
  fs::directory_iterator It(OutputDirectory, EC), End;
  while (!EC && It != End) {
    if (It->is_regular_file(EC))
      Actual.insert(It->path().filename().string());
    if (EC)
      break;
    It.increment(EC);
  }
  return !EC && Actual == Expected;
}

static bool testSequentialReset(const fs::path &Root) {
  std::vector<fs::path> ThinInputs =
      createInputs(Root / "sequential-thin-inputs", "thin", 1);
  std::vector<fs::path> NormalInputs =
      createInputs(Root / "sequential-normal-inputs", "normal", 1);
  if (ThinInputs.size() != 1 || NormalInputs.size() != 1)
    return false;

  fs::path ThinArchive = Root / "sequential-thin.a";
  fs::path NormalArchive = Root / "sequential-normal.a";
  if (invokeAr(archiveArguments("rcT", ThinArchive, ThinInputs)) != 0 ||
      invokeAr(archiveArguments("rc", NormalArchive, NormalInputs)) != 0)
    return false;

  return hasMagic(ThinArchive, "!<thin>\n") &&
         hasMagic(NormalArchive, "!<arch>\n") &&
         extractAndCheck(NormalArchive, Root / "sequential-output",
                         NormalInputs);
}

static bool testConcurrentSameTool(const fs::path &Root) {
  constexpr unsigned InputCount = 32;
  std::vector<fs::path> ThinInputs =
      createInputs(Root / "concurrent-thin-inputs", "thin", InputCount);
  std::vector<fs::path> NormalInputs =
      createInputs(Root / "concurrent-normal-inputs", "normal", InputCount);
  if (ThinInputs.size() != InputCount || NormalInputs.size() != InputCount)
    return false;

  fs::path ThinArchive = Root / "concurrent-thin.a";
  fs::path NormalArchive = Root / "concurrent-normal.a";
  std::vector<std::string> ThinArgs =
      archiveArguments("rcT", ThinArchive, ThinInputs);
  std::vector<std::string> NormalArgs =
      archiveArguments("rc", NormalArchive, NormalInputs);

  std::atomic<unsigned> Ready{0};
  std::atomic<bool> Start{false};
  int ThinResult = -1;
  int NormalResult = -1;
  auto Run = [&](std::vector<std::string> &Args, int &Result) {
    Ready.fetch_add(1, std::memory_order_release);
    while (!Start.load(std::memory_order_acquire))
      std::this_thread::yield();
    Result = invokeAr(Args);
  };

  std::thread ThinThread(Run, std::ref(ThinArgs), std::ref(ThinResult));
  std::thread NormalThread(Run, std::ref(NormalArgs), std::ref(NormalResult));
  while (Ready.load(std::memory_order_acquire) != 2)
    std::this_thread::yield();
  Start.store(true, std::memory_order_release);
  ThinThread.join();
  NormalThread.join();

  return ThinResult == 0 && NormalResult == 0 &&
         hasMagic(ThinArchive, "!<thin>\n") &&
         hasMagic(NormalArchive, "!<arch>\n") &&
         extractAndCheck(NormalArchive, Root / "concurrent-output",
                         NormalInputs);
}

static std::atomic<bool> FirstQuotaConfigured{false};
static std::atomic<bool> SecondQuotaConfigured{false};
static std::atomic<bool> ReleaseQuotaWork{false};
static std::atomic<unsigned> QuotaSubmitted{0};
static std::array<std::atomic<unsigned>, 2> QuotaActive{};
static std::array<std::atomic<unsigned>, 2> QuotaMaximum{};
static std::array<std::atomic<unsigned>, 2> QuotaStarted{};

static void updateMaximum(std::atomic<unsigned> &Maximum, unsigned Value) {
  unsigned OldMaximum = Maximum.load(std::memory_order_relaxed);
  while (Value > OldMaximum &&
         !Maximum.compare_exchange_weak(OldMaximum, Value,
                                        std::memory_order_relaxed)) {
  }
}

static int quotaToolMain(int Argc, char **Argv,
                         const llvm::ToolContext &) {
  if (Argc != 2 || (Argv[1][0] != '0' && Argv[1][0] != '1') || Argv[1][1])
    return 1;
  const unsigned Index = static_cast<unsigned>(Argv[1][0] - '0');
  const unsigned Width = Index + 1;

  if (Index == 0) {
    llvm::parallel::strategy = llvm::hardware_concurrency(Width);
    FirstQuotaConfigured.store(true, std::memory_order_release);
    while (!SecondQuotaConfigured.load(std::memory_order_acquire))
      std::this_thread::yield();
  } else {
    while (!FirstQuotaConfigured.load(std::memory_order_acquire))
      std::this_thread::yield();
    llvm::parallel::strategy = llvm::hardware_concurrency(Width);
    SecondQuotaConfigured.store(true, std::memory_order_release);
  }

  llvm::parallel::TaskGroup Tasks;
  for (unsigned I = 0; I != 8; ++I) {
    Tasks.spawn([Index] {
      unsigned Current =
          QuotaActive[Index].fetch_add(1, std::memory_order_relaxed) + 1;
      updateMaximum(QuotaMaximum[Index], Current);
      QuotaStarted[Index].fetch_add(1, std::memory_order_release);
      while (!ReleaseQuotaWork.load(std::memory_order_acquire))
        std::this_thread::yield();
      QuotaActive[Index].fetch_sub(1, std::memory_order_relaxed);
    });
  }
  QuotaSubmitted.fetch_add(1, std::memory_order_release);
  return 0;
}

static bool testSharedExecutorInvocationQuotas() {
  if (llvm::hardware_concurrency().compute_thread_count() < 3)
    return true;

  FirstQuotaConfigured.store(false, std::memory_order_relaxed);
  SecondQuotaConfigured.store(false, std::memory_order_relaxed);
  ReleaseQuotaWork.store(false, std::memory_order_relaxed);
  QuotaSubmitted.store(0, std::memory_order_relaxed);
  for (unsigned Index = 0; Index != 2; ++Index) {
    QuotaActive[Index].store(0, std::memory_order_relaxed);
    QuotaMaximum[Index].store(0, std::memory_order_relaxed);
    QuotaStarted[Index].store(0, std::memory_order_relaxed);
  }

  auto InvokeQuotaTool = [](const char *Index) {
    return invokeTool(+[] {}, quotaToolMain, {"quota-tool", Index});
  };
  int FirstResult = -1;
  int SecondResult = -1;
  std::thread First([&] { FirstResult = InvokeQuotaTool("0"); });
  std::thread Second([&] { SecondResult = InvokeQuotaTool("1"); });

  while (QuotaSubmitted.load(std::memory_order_acquire) != 2)
    std::this_thread::yield();
  auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ((QuotaStarted[0].load(std::memory_order_acquire) < 1 ||
          QuotaStarted[1].load(std::memory_order_acquire) < 2) &&
         std::chrono::steady_clock::now() < Deadline)
    std::this_thread::yield();
  const bool ReachedExpectedConcurrency =
      QuotaStarted[0].load(std::memory_order_acquire) >= 1 &&
      QuotaStarted[1].load(std::memory_order_acquire) >= 2;
  ReleaseQuotaWork.store(true, std::memory_order_release);
  First.join();
  Second.join();

  return ReachedExpectedConcurrency && FirstResult == 0 &&
         SecondResult == 0 &&
         QuotaMaximum[0].load(std::memory_order_relaxed) == 1 &&
         QuotaMaximum[1].load(std::memory_order_relaxed) <= 2;
}

#ifdef LLVM_DRIVER_TEST_CLANG_TRIPLE
static std::vector<std::string>
clangArguments(const fs::path &Input, const fs::path &Output,
               const char *OptimizationLevel) {
  return {"clang", "-cc1", "-triple", LLVM_DRIVER_TEST_CLANG_TRIPLE,
          "-emit-obj", OptimizationLevel, "-o", Output.string(),
          Input.string()};
}

static std::vector<std::string>
pluginClangArguments(const fs::path &Input, const fs::path &Output,
                     const fs::path &Plugin, const fs::path &Trace,
                     const char *Value = nullptr, bool Synchronize = false) {
  std::vector<std::string> Args{
      "clang",
      "-cc1",
      "-triple",
      LLVM_DRIVER_TEST_CLANG_TRIPLE,
      "-emit-obj",
      "-fpass-plugin=" + Plugin.string(),
  };
  if (Value)
    Args.insert(Args.end(),
                {"-mllvm", std::string("-per-invocation-plugin-value=") +
                               Value});
  if (Synchronize)
    Args.insert(Args.end(),
                {"-mllvm", "-per-invocation-plugin-synchronize"});
  Args.insert(Args.end(),
              {"-mllvm", "-per-invocation-plugin-trace=" + Trace.string(),
               "-o", Output.string(), Input.string()});
  return Args;
}
#endif

static bool isNonEmptyFile(const fs::path &Path) {
  std::error_code EC;
  uintmax_t Size = fs::file_size(Path, EC);
  return !EC && Size != 0;
}

#ifdef LLVM_DRIVER_TEST_CLANG_TRIPLE
static bool hasContents(const fs::path &Path, const char *Expected) {
  std::ifstream IS(Path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(IS),
                     std::istreambuf_iterator<char>()) == Expected;
}

static bool testPluginConcurrentIsolation(const fs::path &Root,
                                          const fs::path &Plugin,
                                          const fs::path &Input) {
  fs::path LeftOutput = Root / "plugin-left.o";
  fs::path RightOutput = Root / "plugin-right.o";
  fs::path LeftTrace = Root / "plugin-left.trace";
  fs::path RightTrace = Root / "plugin-right.trace";
  std::vector<std::string> LeftArgs = pluginClangArguments(
      Input, LeftOutput, Plugin, LeftTrace, "left", true);
  std::vector<std::string> RightArgs = pluginClangArguments(
      Input, RightOutput, Plugin, RightTrace, "right", true);

  std::atomic<unsigned> Ready{0};
  std::atomic<bool> Start{false};
  int LeftResult = -1;
  int RightResult = -1;
  auto Run = [&](std::vector<std::string> &Args, int &Result) {
    Ready.fetch_add(1, std::memory_order_release);
    while (!Start.load(std::memory_order_acquire))
      std::this_thread::yield();
    Result = invokeClang(Args);
  };

  std::thread LeftThread(Run, std::ref(LeftArgs), std::ref(LeftResult));
  std::thread RightThread(Run, std::ref(RightArgs), std::ref(RightResult));
  while (Ready.load(std::memory_order_acquire) != 2)
    std::this_thread::yield();
  Start.store(true, std::memory_order_release);
  LeftThread.join();
  RightThread.join();

  return LeftResult == 0 && RightResult == 0 && isNonEmptyFile(LeftOutput) &&
         isNonEmptyFile(RightOutput) &&
         hasContents(LeftTrace, "run=left\ndestroy=left\n") &&
         hasContents(RightTrace, "run=right\ndestroy=right\n");
}

static bool testPluginSequentialReset(const fs::path &Root,
                                      const fs::path &Plugin,
                                      const fs::path &Input) {
  fs::path FirstOutput = Root / "plugin-first.o";
  fs::path DefaultOutput = Root / "plugin-default.o";
  fs::path FirstTrace = Root / "plugin-first.trace";
  fs::path DefaultTrace = Root / "plugin-default.trace";
  if (invokeClang(pluginClangArguments(Input, FirstOutput, Plugin, FirstTrace,
                                      "first")) != 0 ||
      invokeClang(pluginClangArguments(Input, DefaultOutput, Plugin,
                                      DefaultTrace)) != 0)
    return false;

  return isNonEmptyFile(FirstOutput) && isNonEmptyFile(DefaultOutput) &&
         hasContents(FirstTrace, "run=first\ndestroy=first\n") &&
         hasContents(DefaultTrace, "run=default\ndestroy=default\n");
}

static bool testPerInvocationPlugin(const fs::path &Root,
                                    const fs::path &Plugin) {
  std::error_code EC;
  fs::create_directories(Root, EC);
  fs::path Input = Root / "plugin-input.c";
  if (EC || !writeFile(Input, "int plugin_input(void) { return 0; }\n"))
    return false;

  // Target and pass registration are immutable process catalogs. Initialize
  // them before racing the two invocation-owned plugin option sets. Loading
  // the DSO is also process-wide and serialized; PassPlugin::Load will still
  // call its entry point inside every invocation below.
  initializeProcessCatalogs();
  std::string LoadError;
  llvm::sys::DynamicLibrary Library =
      llvm::sys::DynamicLibrary::getPermanentLibrary(Plugin.string().c_str(),
                                                      &LoadError);
  if (!Library.isValid()) {
    std::fprintf(stderr, "could not preload plugin: %s\n", LoadError.c_str());
    return false;
  }
  if (!testPluginConcurrentIsolation(Root, Plugin, Input)) {
    std::fputs("concurrent plugin option isolation failed\n", stderr);
    return false;
  }
  std::puts("plugin-concurrent-isolation=ok");

  if (!testPluginSequentialReset(Root, Plugin, Input)) {
    std::fputs("sequential plugin option reset/destruction failed\n", stderr);
    return false;
  }
  std::puts("plugin-sequential-reset-and-destruction=ok");
  return true;
}

static bool testConcurrentClang(const fs::path &Root) {
  fs::path FirstInput = Root / "clang-first.c";
  fs::path SecondInput = Root / "clang-second.c";
  fs::path FirstOutput = Root / "clang-first.o";
  fs::path SecondOutput = Root / "clang-second.o";
  fs::path ReusedOutput = Root / "clang-reused.o";
  if (!writeFile(FirstInput, "int first(void) { return 1; }\n") ||
      !writeFile(SecondInput, "int second(void) { return 2; }\n"))
    return false;

  std::vector<std::string> FirstArgs =
      clangArguments(FirstInput, FirstOutput, "-O0");
  std::vector<std::string> SecondArgs =
      clangArguments(SecondInput, SecondOutput, "-O2");
  std::atomic<unsigned> Ready{0};
  std::atomic<bool> Start{false};
  int FirstResult = -1;
  int SecondResult = -1;
  auto Run = [&](std::vector<std::string> &Args, int &Result) {
    Ready.fetch_add(1, std::memory_order_release);
    while (!Start.load(std::memory_order_acquire))
      std::this_thread::yield();
    Result = invokeClang(Args);
  };

  std::thread FirstThread(Run, std::ref(FirstArgs), std::ref(FirstResult));
  std::thread SecondThread(Run, std::ref(SecondArgs), std::ref(SecondResult));
  while (Ready.load(std::memory_order_acquire) != 2)
    std::this_thread::yield();
  Start.store(true, std::memory_order_release);
  FirstThread.join();
  SecondThread.join();

  if (FirstResult != 0 || SecondResult != 0 ||
      !isNonEmptyFile(FirstOutput) || !isNonEmptyFile(SecondOutput))
    return false;
  return invokeClang(clangArguments(FirstInput, ReusedOutput, "-O1")) == 0 &&
         isNonEmptyFile(ReusedOutput);
}
#endif

static std::vector<std::string>
machOArguments(const fs::path &Input, const fs::path &Output,
               const fs::path &Map) {
  return {"ld64.lld",        "-arch", "x86_64", "-platform_version",
          "macos",           "10.15", "10.15",   "--read-workers=2",
          "-e",              "_main", "-map",    Map.string(),
          "-o",              Output.string(),
          Input.string()};
}

static std::vector<std::string>
wasmArguments(const fs::path &Input, const fs::path &Output) {
  return {"wasm-ld", "--no-entry", Input.string(), "-o", Output.string()};
}

static bool invokeLldConcurrently(std::vector<std::string> FirstArgs,
                                  std::vector<std::string> SecondArgs) {
  std::atomic<unsigned> Ready{0};
  std::atomic<bool> Start{false};
  int FirstResult = -1;
  int SecondResult = -1;
  auto Run = [&](std::vector<std::string> &Args, int &Result) {
    Ready.fetch_add(1, std::memory_order_release);
    while (!Start.load(std::memory_order_acquire))
      std::this_thread::yield();
    Result = invokeLld(Args);
  };

  std::thread FirstThread(Run, std::ref(FirstArgs), std::ref(FirstResult));
  std::thread SecondThread(Run, std::ref(SecondArgs), std::ref(SecondResult));
  while (Ready.load(std::memory_order_acquire) != 2)
    std::this_thread::yield();
  Start.store(true, std::memory_order_release);
  FirstThread.join();
  SecondThread.join();
  return FirstResult == 0 && SecondResult == 0;
}

static bool testLldSequentialAndConcurrent(const fs::path &Root,
                                           const fs::path &MachOInput,
                                           const fs::path &WasmInput) {
  fs::path MachOSequentialOne = Root / "macho-sequential-one";
  fs::path MachOSequentialTwo = Root / "macho-sequential-two";
  fs::path WasmSequentialOne = Root / "wasm-sequential-one.wasm";
  fs::path WasmSequentialTwo = Root / "wasm-sequential-two.wasm";
  if (invokeLld(machOArguments(MachOInput, MachOSequentialOne,
                               Root / "macho-sequential-one.map")) != 0 ||
      invokeLld(machOArguments(MachOInput, MachOSequentialTwo,
                               Root / "macho-sequential-two.map")) != 0 ||
      invokeLld(wasmArguments(WasmInput, WasmSequentialOne)) != 0 ||
      invokeLld(wasmArguments(WasmInput, WasmSequentialTwo)) != 0 ||
      !isNonEmptyFile(MachOSequentialOne) ||
      !isNonEmptyFile(MachOSequentialTwo) ||
      !isNonEmptyFile(WasmSequentialOne) ||
      !isNonEmptyFile(WasmSequentialTwo))
    return false;

  fs::path MachOConcurrentOne = Root / "macho-concurrent-one";
  fs::path MachOConcurrentTwo = Root / "macho-concurrent-two";
  fs::path WasmConcurrentOne = Root / "wasm-concurrent-one.wasm";
  fs::path WasmConcurrentTwo = Root / "wasm-concurrent-two.wasm";
  return invokeLldConcurrently(
             machOArguments(MachOInput, MachOConcurrentOne,
                            Root / "macho-concurrent-one.map"),
             machOArguments(MachOInput, MachOConcurrentTwo,
                            Root / "macho-concurrent-two.map")) &&
         invokeLldConcurrently(wasmArguments(WasmInput, WasmConcurrentOne),
                               wasmArguments(WasmInput, WasmConcurrentTwo)) &&
         isNonEmptyFile(MachOConcurrentOne) &&
         isNonEmptyFile(MachOConcurrentTwo) &&
         isNonEmptyFile(WasmConcurrentOne) &&
         isNonEmptyFile(WasmConcurrentTwo);
}

} // namespace

int main(int Argc, char **Argv) {
  llvm::InitLLVM X(Argc, Argv);
  if (Argc == 2 && std::string(Argv[1]) == "--plugin-support") {
#ifdef LLVM_DRIVER_TEST_CLANG_TRIPLE
    return 0;
#else
    return 1;
#endif
  }
  if (Argc == 4 && std::string(Argv[1]) == "--plugin") {
#ifdef LLVM_DRIVER_TEST_CLANG_TRIPLE
    return testPerInvocationPlugin(Argv[3], Argv[2]) ? 0 : 1;
#else
    std::fputs("folded Clang support is unavailable\n", stderr);
    return 1;
#endif
  }
  if (Argc != 4) {
    std::fprintf(stderr,
                 "usage: %s <temporary-directory> <Mach-O-object> "
                 "<Wasm-object>\n",
                 Argv[0]);
    return 1;
  }

  if (!testColdProcessInitialization()) {
    std::fputs("cold process initialization failed\n", stderr);
    return 1;
  }
  std::puts("cold-process-initialization=ok");

  fs::path Root(Argv[1]);
  if (!testSequentialReset(Root)) {
    std::fputs("folded llvm-ar sequential reset failed\n", stderr);
    return 1;
  }
  std::puts("sequential-reset=ok");

  if (!testConcurrentSameTool(Root)) {
    std::fputs("concurrent folded llvm-ar invocation failed\n", stderr);
    return 1;
  }
  std::puts("concurrent-same-tool=ok");

  if (!testSharedExecutorInvocationQuotas()) {
    std::fputs("shared executor invocation quotas failed\n", stderr);
    return 1;
  }
  std::puts("shared-executor-quotas=ok");

#ifdef LLVM_DRIVER_TEST_CLANG_TRIPLE
  if (!testConcurrentClang(Root)) {
    std::fputs("concurrent folded Clang invocation failed\n", stderr);
    return 1;
  }
  std::puts("clang-concurrent=ok");
#else
  std::puts("clang-concurrent=unavailable");
#endif

  if (!testLldSequentialAndConcurrent(Root, Argv[2], Argv[3])) {
    std::fputs("folded LLD sequential/concurrent invocation failed\n", stderr);
    return 1;
  }
  std::puts("lld-sequential-concurrent=ok");
  return 0;
}
