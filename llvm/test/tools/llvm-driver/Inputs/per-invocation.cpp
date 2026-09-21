//===- per-invocation.cpp - Folded llvm-ar lifecycle smoke test -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Driver.h"
#include "llvm/Support/TargetSelect.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

int llvm_ar_main(int Argc, char **Argv, const llvm::ToolContext &);
void llvm_ar_init_lifecycle();
int lld_main(int Argc, char **Argv, const llvm::ToolContext &);
void lld_init_lifecycle();

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

static bool isNonEmptyFile(const fs::path &Path) {
  std::error_code EC;
  uintmax_t Size = fs::file_size(Path, EC);
  return !EC && Size != 0;
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
  if (Argc != 4) {
    std::fprintf(stderr,
                 "usage: %s <temporary-directory> <Mach-O-object> "
                 "<Wasm-object>\n",
                 Argv[0]);
    return 1;
  }

  // llvm.exe performs process-wide target registration before two folded
  // invocations can overlap. Match the subset llvm-ar itself consumes.
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();

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

  if (!testLldSequentialAndConcurrent(Root, Argv[2], Argv[3])) {
    std::fputs("folded LLD sequential/concurrent invocation failed\n", stderr);
    return 1;
  }
  std::puts("lld-sequential-concurrent=ok");
  return 0;
}
