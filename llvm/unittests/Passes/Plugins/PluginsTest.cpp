//===- unittests/Passes/Plugins/PluginsTest.cpp ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Config/config.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Testing/Support/Error.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "gtest/gtest.h"

#include "TestPlugin.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>

using namespace llvm;

void anchor() {}

static std::string LibPath(const std::string Name = "TestPlugin") {
  const auto &Argvs = testing::internal::GetArgvs();
  const char *Argv0 = Argvs.size() > 0 ? Argvs[0].c_str() : "PluginsTests";
  void *Ptr = (void *)(intptr_t)anchor;
  std::string Path = sys::fs::getMainExecutable(Argv0, Ptr);
  llvm::SmallString<256> Buf{sys::path::parent_path(Path)};
  sys::path::append(Buf, (Name + LLVM_PLUGIN_EXT).c_str());
  return std::string(Buf.str());
}

template <class T> static T functionPointer(void *Ptr) {
  union {
    T Function;
    void *Pointer;
  } Value;
  Value.Pointer = Ptr;
  return Value.Function;
}

struct DynamicPluginAPI {
  sys::DynamicLibrary Library;
  std::string Path;
  std::string LoadError;
  TestPluginOptionValueFn Value = nullptr;
  TestPluginOptionAddressFn Address = nullptr;
  TestPluginOptionCountFn ConstructionCount = nullptr;
  TestPluginOptionCountFn DestructionCount = nullptr;
  TestPluginOptionCountFn PostOptionDestructionCount = nullptr;

  explicit DynamicPluginAPI(StringRef PluginPath) : Path(PluginPath) {
    // Keep the DSO loaded independently of any one PassPlugin object or tool
    // invocation. Loading the image constructs only its process-wide
    // ContextManagedStatic key; PassPlugin::Load below creates the option in
    // the command-line context which is already parsing.
    Library =
        sys::DynamicLibrary::getPermanentLibrary(Path.c_str(), &LoadError);
    if (!Library.isValid())
      return;
    Value = functionPointer<TestPluginOptionValueFn>(
        Library.getAddressOfSymbol(TEST_PLUGIN_OPTION_VALUE_SYMBOL));
    Address = functionPointer<TestPluginOptionAddressFn>(
        Library.getAddressOfSymbol(TEST_PLUGIN_OPTION_ADDRESS_SYMBOL));
    ConstructionCount = functionPointer<TestPluginOptionCountFn>(
        Library.getAddressOfSymbol(TEST_PLUGIN_CONSTRUCTION_COUNT_SYMBOL));
    DestructionCount = functionPointer<TestPluginOptionCountFn>(
        Library.getAddressOfSymbol(TEST_PLUGIN_DESTRUCTION_COUNT_SYMBOL));
    PostOptionDestructionCount =
        functionPointer<TestPluginOptionCountFn>(Library.getAddressOfSymbol(
            TEST_PLUGIN_POST_OPTION_DESTRUCTION_COUNT_SYMBOL));
  }

  bool isValid() const {
    return Library.isValid() && Value && Address && ConstructionCount &&
           DestructionCount && PostOptionDestructionCount;
  }
};

static DynamicPluginAPI &dynamicPluginAPI() {
  static DynamicPluginAPI API(LibPath());
  return API;
}

struct DynamicOptionInvocationResult {
  bool RegisteredBeforeLoad = false;
  bool RegisteredAfterLoad = false;
  bool Parsed = false;
  bool Loaded = false;
  std::string LoadError;
  std::string Diagnostics;
  std::string Value;
  const void *Address = nullptr;
};

// Runs inside an already-pushed cl::ScopedContext. In particular, the pass
// plugin is not loaded before ParseCommandLineOptions: its loader option's
// callback calls PassPlugin::Load while the parser is walking argv, and the
// plugin option immediately following it must be found in the updated map.
static DynamicOptionInvocationResult
parseDynamicPluginOption(DynamicPluginAPI &API, StringRef RequestedValue) {
  DynamicOptionInvocationResult Result;
  Result.RegisteredBeforeLoad =
      cl::getRegisteredOptions().contains(TEST_PLUGIN_OPTION_NAME);

  std::optional<PassPlugin> LoadedPlugin;
  cl::opt<std::string> Loader(
      "load-test-pass-plugin",
      cl::desc("Load the command-line context test plugin"),
      cl::callback([&](const std::string &PluginPath) {
        Expected<PassPlugin> Plugin = PassPlugin::Load(PluginPath);
        if (!Plugin) {
          Result.LoadError = toString(Plugin.takeError());
          return;
        }
        LoadedPlugin.emplace(std::move(*Plugin));
      }));

  std::string LoadArgument = "--load-test-pass-plugin=" + API.Path;
  std::string ValueArgument =
      (Twine("--") + TEST_PLUGIN_OPTION_NAME + "=" + RequestedValue).str();
  const char *Args[] = {"plugin-options-test", LoadArgument.c_str(),
                        ValueArgument.c_str()};
  raw_string_ostream Diagnostics(Result.Diagnostics);
  Result.Parsed = cl::ParseCommandLineOptions(std::size(Args), Args,
                                              StringRef(), &Diagnostics);
  Diagnostics.flush();
  Result.Loaded = LoadedPlugin.has_value();
  Result.RegisteredAfterLoad =
      cl::getRegisteredOptions().contains(TEST_PLUGIN_OPTION_NAME);
  if (Result.Parsed && Result.Loaded) {
    Result.Value = API.Value();
    Result.Address = API.Address();
  }
  return Result;
}

static void expectSuccessfulDynamicOptionInvocation(
    const DynamicOptionInvocationResult &Result, StringRef ExpectedValue) {
  EXPECT_FALSE(Result.RegisteredBeforeLoad);
  EXPECT_TRUE(Result.RegisteredAfterLoad);
  EXPECT_TRUE(Result.Loaded) << Result.LoadError;
  EXPECT_TRUE(Result.Parsed) << Result.Diagnostics;
  EXPECT_EQ(ExpectedValue, Result.Value);
  EXPECT_NE(nullptr, Result.Address);
}

TEST(PluginsTests, LoadPlugin) {
#if !defined(LLVM_ENABLE_PLUGINS)
  // Skip the test if plugins are disabled.
  GTEST_SKIP();
#endif

  auto PluginPath = LibPath();
  ASSERT_NE("", PluginPath);

  Expected<PassPlugin> Plugin = PassPlugin::Load(PluginPath);
  ASSERT_TRUE(!!Plugin) << "Plugin path: " << PluginPath;

  ASSERT_EQ(TEST_PLUGIN_NAME, Plugin->getPluginName());
  ASSERT_EQ(TEST_PLUGIN_VERSION, Plugin->getPluginVersion());

  PassBuilder PB;
  ModulePassManager PM;
  ASSERT_THAT_ERROR(PB.parsePassPipeline(PM, "plugin-pass"), Failed());

  Plugin->registerPassBuilderCallbacks(PB);
  ASSERT_THAT_ERROR(PB.parsePassPipeline(PM, "plugin-pass"), Succeeded());
}

TEST(PluginsTests, DynamicOptionsResetAcrossSequentialContexts) {
#if !defined(LLVM_ENABLE_PLUGINS)
  GTEST_SKIP();
#endif

  DynamicPluginAPI &API = dynamicPluginAPI();
  ASSERT_TRUE(API.isValid())
      << "Plugin path: " << API.Path << ": " << API.LoadError;

  const unsigned InitialConstructions = API.ConstructionCount();
  const unsigned InitialDestructions = API.DestructionCount();
  const unsigned InitialPostOptionDestructions =
      API.PostOptionDestructionCount();

  {
    cl::ScopedContext Context;
    DynamicOptionInvocationResult First =
        parseDynamicPluginOption(API, "first-value");
    expectSuccessfulDynamicOptionInvocation(First, "first-value");
  }
  EXPECT_EQ(InitialConstructions + 1, API.ConstructionCount());
  EXPECT_EQ(InitialDestructions + 1, API.DestructionCount());
  EXPECT_EQ(InitialPostOptionDestructions + 1, API.PostOptionDestructionCount())
      << "the observer must run after cl::opt<std::string> is destroyed and "
         "unregistered";

  {
    cl::ScopedContext Context;
    DynamicOptionInvocationResult Second =
        parseDynamicPluginOption(API, "second-value");
    expectSuccessfulDynamicOptionInvocation(Second, "second-value");
  }
  EXPECT_EQ(InitialConstructions + 2, API.ConstructionCount());
  EXPECT_EQ(InitialDestructions + 2, API.DestructionCount());
  EXPECT_EQ(InitialPostOptionDestructions + 2,
            API.PostOptionDestructionCount());
}

TEST(PluginsTests, DynamicOptionsFollowPropagatedContext) {
#if !defined(LLVM_ENABLE_PLUGINS)
  GTEST_SKIP();
#endif

  DynamicPluginAPI &API = dynamicPluginAPI();
  ASSERT_TRUE(API.isValid())
      << "Plugin path: " << API.Path << ": " << API.LoadError;

  const unsigned InitialDestructions = API.DestructionCount();
  const unsigned InitialPostOptionDestructions =
      API.PostOptionDestructionCount();
  {
    cl::ScopedContext Context;
    DynamicOptionInvocationResult Owner =
        parseDynamicPluginOption(API, "worker-value");
    expectSuccessfulDynamicOptionInvocation(Owner, "worker-value");

    std::string WorkerValue;
    const void *WorkerAddress = nullptr;
    cl::ContextToken Token = cl::ContextToken::capture();
    std::thread Worker([&API, Token = std::move(Token), &WorkerValue,
                        &WorkerAddress]() mutable {
      cl::ScopedContextTokenBinding Binding(std::move(Token));
      WorkerValue = API.Value();
      WorkerAddress = API.Address();
    });
    Worker.join();

    EXPECT_EQ("worker-value", WorkerValue);
    EXPECT_EQ(Owner.Address, WorkerAddress)
        << "a propagated worker must use the owner's plugin option instance";
  }
  EXPECT_EQ(InitialDestructions + 1, API.DestructionCount());
  EXPECT_EQ(InitialPostOptionDestructions + 1,
            API.PostOptionDestructionCount());
}

TEST(PluginsTests, DynamicOptionsAreDistinctAcrossConcurrentContexts) {
#if !defined(LLVM_ENABLE_PLUGINS)
  GTEST_SKIP();
#endif

  DynamicPluginAPI &API = dynamicPluginAPI();
  ASSERT_TRUE(API.isValid())
      << "Plugin path: " << API.Path << ": " << API.LoadError;

  const unsigned InitialConstructions = API.ConstructionCount();
  const unsigned InitialDestructions = API.DestructionCount();
  const unsigned InitialPostOptionDestructions =
      API.PostOptionDestructionCount();
  std::array<DynamicOptionInvocationResult, 2> Results;
  std::atomic<unsigned> Ready{0};
  std::atomic<bool> Release{false};

  auto Invoke = [&](unsigned Index, StringRef Value) {
    cl::ScopedContext Context;
    Results[Index] = parseDynamicPluginOption(API, Value);
    Ready.fetch_add(1, std::memory_order_release);
    while (!Release.load(std::memory_order_acquire))
      std::this_thread::yield();
  };

  std::thread First(Invoke, 0, StringRef("concurrent-first"));
  std::thread Second(Invoke, 1, StringRef("concurrent-second"));
  while (Ready.load(std::memory_order_acquire) != 2)
    std::this_thread::yield();

  expectSuccessfulDynamicOptionInvocation(Results[0], "concurrent-first");
  expectSuccessfulDynamicOptionInvocation(Results[1], "concurrent-second");
  EXPECT_NE(Results[0].Address, Results[1].Address)
      << "overlapping invocations must not share plugin option storage";

  Release.store(true, std::memory_order_release);
  First.join();
  Second.join();

  EXPECT_EQ(InitialConstructions + 2, API.ConstructionCount());
  EXPECT_EQ(InitialDestructions + 2, API.DestructionCount());
  EXPECT_EQ(InitialPostOptionDestructions + 2,
            API.PostOptionDestructionCount());
}

// Test that llvmGetPassPluginInfo from DoublerPlugin is called twice with
// -fpass-plugin=DoublerPlugin -fpass-plugin=TestPlugin
// -fpass-plugin=DoublerPlugin.
TEST(PluginsTests, LoadMultiplePlugins) {
#if !defined(LLVM_ENABLE_PLUGINS)
  // Skip the test if plugins are disabled.
  GTEST_SKIP();
#endif

  auto DoublerPluginPath = LibPath("DoublerPlugin");
  auto TestPluginPath = LibPath("TestPlugin");
  ASSERT_NE("", DoublerPluginPath);
  ASSERT_NE("", TestPluginPath);

  Expected<PassPlugin> DoublerPlugin1 = PassPlugin::Load(DoublerPluginPath);
  ASSERT_TRUE(!!DoublerPlugin1)
      << "Plugin path: " << DoublerPlugin1->getFilename();

  Expected<PassPlugin> TestPlugin = PassPlugin::Load(TestPluginPath);
  ASSERT_TRUE(!!TestPlugin) << "Plugin path: " << TestPlugin->getFilename();

  // If llvmGetPassPluginInfo is resolved as a weak symbol taking into account
  // all loaded symbols, the second call to PassPlugin::Load will actually
  // return the llvmGetPassPluginInfo from the most recently loaded plugin, in
  // this case TestPlugin.
  Expected<PassPlugin> DoublerPlugin2 = PassPlugin::Load(DoublerPluginPath);
  ASSERT_TRUE(!!DoublerPlugin2)
      << "Plugin path: " << DoublerPlugin2->getFilename();

  ASSERT_EQ("DoublerPlugin", DoublerPlugin1->getPluginName());
  ASSERT_EQ("2.2-unit", DoublerPlugin1->getPluginVersion());
  ASSERT_EQ(TEST_PLUGIN_NAME, TestPlugin->getPluginName());
  ASSERT_EQ(TEST_PLUGIN_VERSION, TestPlugin->getPluginVersion());
  // Check that the plugin name/version is set correctly when loaded a second
  // time
  ASSERT_EQ("DoublerPlugin", DoublerPlugin2->getPluginName());
  ASSERT_EQ("2.2-unit", DoublerPlugin2->getPluginVersion());

  PassBuilder PB;
  ModulePassManager PM;
  const char *PipelineText = "module(doubler-pass,plugin-pass,doubler-pass)";
  ASSERT_THAT_ERROR(PB.parsePassPipeline(PM, PipelineText), Failed());
  TestPlugin->registerPassBuilderCallbacks(PB);
  DoublerPlugin1->registerPassBuilderCallbacks(PB);
  DoublerPlugin2->registerPassBuilderCallbacks(PB);
  ASSERT_THAT_ERROR(PB.parsePassPipeline(PM, PipelineText), Succeeded());

  LLVMContext C;
  SMDiagnostic Err;
  std::unique_ptr<Module> M =
      parseAssemblyString(R"IR(@doubleme = constant i32 7)IR", Err, C);

  // Check that the initial value is 7
  {
    auto *GV = M->getNamedValue("doubleme");
    auto *Init = cast<GlobalVariable>(GV)->getInitializer();
    auto *CI = cast<ConstantInt>(Init);
    ASSERT_EQ(CI->getSExtValue(), 7);
  }

  ModuleAnalysisManager MAM;
  // Register required pass instrumentation analysis.
  MAM.registerPass([&] { return PassInstrumentationAnalysis(); });
  PM.run(*M, MAM);

  // Check that the final value is 28 because DoublerPlugin::run was called
  // twice, indicating that the llvmGetPassPluginInfo and registerCallbacks
  // were correctly called.
  {
    // Check the value was doubled twice
    auto *GV = M->getNamedValue("doubleme");
    auto *Init = cast<GlobalVariable>(GV)->getInitializer();
    auto *CI = cast<ConstantInt>(Init);
    ASSERT_EQ(CI->getSExtValue(), 28);
  }
}
