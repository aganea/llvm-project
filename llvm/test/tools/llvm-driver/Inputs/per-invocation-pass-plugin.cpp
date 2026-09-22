//===- per-invocation-pass-plugin.cpp - Dynamic option test plugin --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ManagedStatic.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#if LLVM_ENABLE_THREADS
#include <thread>
#endif

using namespace llvm;

namespace {

static void appendTrace(const std::string &Path, const char *Event,
                        const std::string &Value) {
  if (Path.empty())
    return;
  if (std::FILE *File = std::fopen(Path.c_str(), "ab")) {
    std::fprintf(File, "%s=%s\n", Event, Value.c_str());
    std::fclose(File);
  }
}

/// The key lives for as long as the permanently loaded plugin, while each
/// value belongs to the command-line context active when the plugin is loaded.
/// In particular, both std::string option values must be destroyed rather than
/// abandoned when the folded tool invocation ends.
struct PluginOptions {
  cl::opt<std::string> Value{"per-invocation-plugin-value",
                             cl::init("default")};
  cl::opt<std::string> Trace{"per-invocation-plugin-trace"};
  cl::opt<bool> Synchronize{"per-invocation-plugin-synchronize",
                            cl::init(false)};

  ~PluginOptions() {
    appendTrace(Trace.getValue(), "destroy", Value.getValue());
  }
};

static ContextManagedStatic<PluginOptions> Options;
static std::atomic<unsigned> SynchronizedCallbacks{0};

static bool synchronizeCallbacks() {
  SynchronizedCallbacks.fetch_add(1, std::memory_order_release);
  const auto Deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (SynchronizedCallbacks.load(std::memory_order_acquire) < 2) {
    if (std::chrono::steady_clock::now() >= Deadline)
      return false;
#if LLVM_ENABLE_THREADS
    std::this_thread::yield();
#endif
  }
  return true;
}

static bool traceCodeGen(Module &, TargetMachine &, CodeGenFileType,
                         raw_pwrite_stream &) {
  PluginOptions &InvocationOptions = *Options;
  if (InvocationOptions.Synchronize && !synchronizeCallbacks())
    return false;
  appendTrace(InvocationOptions.Trace.getValue(), "run",
              InvocationOptions.Value.getValue());
  return false;
}

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  // PassPlugin::Load calls this entry point inside the invocation context.
  // Constructing PluginOptions here makes the new options visible to the
  // -mllvm parse that follows plugin loading.
  (void)*Options;
  return {LLVM_PLUGIN_API_VERSION, "PerInvocationPlugin", LLVM_VERSION_STRING,
          nullptr, traceCodeGen};
}
