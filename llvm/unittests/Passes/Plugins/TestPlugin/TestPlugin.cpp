//===- unittests/Passes/TestPlugin.cpp --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ManagedStatic.h"

#include "../TestPlugin.h"

#include <atomic>
#include <string>

using namespace llvm;

namespace {

std::atomic<unsigned> DynamicOptionConstructions{0};
std::atomic<unsigned> DynamicOptionDestructions{0};
std::atomic<unsigned> DynamicOptionPostOptionDestructions{0};

// Declared before PluginOptions::Option so that its destructor runs after the
// option, including the option's internally stored std::string. This makes the
// exported destruction count evidence that ordinary C++ member destruction
// completed rather than merely that the context released a bump allocation.
struct DynamicOptionDestructionObserver {
  ~DynamicOptionDestructionObserver() {
    if (!cl::getRegisteredOptions().contains(TEST_PLUGIN_OPTION_NAME))
      DynamicOptionPostOptionDestructions.fetch_add(1,
                                                    std::memory_order_relaxed);
    DynamicOptionDestructions.fetch_add(1, std::memory_order_relaxed);
  }
};

struct PluginOptions {
  DynamicOptionDestructionObserver DestructionObserver;
  cl::opt<std::string> Option{TEST_PLUGIN_OPTION_NAME,
                              cl::desc("Test dynamically loaded plugin state")};

  PluginOptions() {
    DynamicOptionConstructions.fetch_add(1, std::memory_order_relaxed);
  }
};

// The key is process-wide, but its PluginOptions instance belongs to whichever
// command-line context calls the plugin entry point. PassPlugin::Load invokes
// that entry point even when this permanently loaded DSO was loaded earlier.
ContextManagedStatic<PluginOptions> DynamicOptions;

} // namespace

struct TestModulePass : public OptionalPassInfoMixin<TestModulePass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
    return PreservedAnalyses::all();
  }

  static void registerCallbacks(PassBuilder &PB) {
    PB.registerPipelineParsingCallback(
        [](StringRef Name, ModulePassManager &PM,
           ArrayRef<PassBuilder::PipelineElement> InnerPipeline) {
          if (Name == "plugin-pass") {
            PM.addPass(TestModulePass());
            return true;
          }
          return false;
        });
  }
};

extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK
llvmGetPassPluginInfo() {
  // Register this plugin's options in the context which is currently parsing
  // its loader option. Repeated and concurrent loads get separate instances.
  (void)*DynamicOptions;
  return {LLVM_PLUGIN_API_VERSION, TEST_PLUGIN_NAME, TEST_PLUGIN_VERSION,
          TestModulePass::registerCallbacks};
}

extern "C" LLVM_ATTRIBUTE_VISIBILITY_DEFAULT const char *
testPluginDynamicOptionValue() {
  return DynamicOptions->Option.getValue().c_str();
}

extern "C" LLVM_ATTRIBUTE_VISIBILITY_DEFAULT const void *
testPluginDynamicOptionAddress() {
  return &DynamicOptions->Option;
}

extern "C" LLVM_ATTRIBUTE_VISIBILITY_DEFAULT unsigned
testPluginDynamicOptionConstructionCount() {
  return DynamicOptionConstructions.load(std::memory_order_relaxed);
}

extern "C" LLVM_ATTRIBUTE_VISIBILITY_DEFAULT unsigned
testPluginDynamicOptionDestructionCount() {
  return DynamicOptionDestructions.load(std::memory_order_relaxed);
}

extern "C" LLVM_ATTRIBUTE_VISIBILITY_DEFAULT unsigned
testPluginDynamicOptionPostOptionDestructionCount() {
  return DynamicOptionPostOptionDestructions.load(std::memory_order_relaxed);
}
