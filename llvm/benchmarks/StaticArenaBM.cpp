//===- StaticArenaBM.cpp - Static-arena creation benchmark ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/StaticArena.h"

#include "benchmark/benchmark.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>

using namespace llvm;

namespace {

#if defined(_WIN32) || defined(__ELF__) || defined(__wasm__)
constexpr size_t ZeroFilledBytes = 1024 * 1024;
constexpr size_t TemplateBytes = 64 * 1024;
static const std::array<uint8_t, TemplateBytes> Template = {1};

static LLVMStaticArenaVarV1 ZeroFilledRecord = {
    UINT64_MAX, ZeroFilledBytes, 64, nullptr, "StaticArenaBM.ZeroFilledRecord"};
static LLVMStaticArenaVarV1 TemplateRecord = {
    UINT64_MAX, TemplateBytes, alignof(uint64_t), Template.data(),
    "StaticArenaBM.TemplateRecord"};
#endif

} // namespace

#if defined(_MSC_VER)
#pragma section(".llvma$v1$b", read)
#if defined(_M_IX86)
#define STATIC_ARENA_BENCHMARK_FORCE_INCLUDE(Name)                             \
  __pragma(comment(linker, "/include:_" #Name))
#else
#define STATIC_ARENA_BENCHMARK_FORCE_INCLUDE(Name)                             \
  __pragma(comment(linker, "/include:" #Name))
#endif
#define STATIC_ARENA_BENCHMARK_ENTRY(Name, Record)                             \
  extern "C" __declspec(allocate(".llvma$v1$b"))                               \
  LLVMStaticArenaVarV1 *const Name = &(Record);                                \
  STATIC_ARENA_BENCHMARK_FORCE_INCLUDE(Name)
#elif defined(_WIN32)
#define STATIC_ARENA_BENCHMARK_ENTRY(Name, Record)                             \
  extern "C" __attribute__((section(".llvma$v1$b"), used))                     \
  LLVMStaticArenaVarV1 *const Name = &(Record)
#elif defined(__ELF__) || defined(__wasm__)
#define STATIC_ARENA_BENCHMARK_ENTRY(Name, Record)                             \
  extern "C" LLVM_ATTRIBUTE_RETAIN __attribute__((section("llvma_v1"), used))  \
  LLVMStaticArenaVarV1 *const Name = &(Record)
#else
#define STATIC_ARENA_BENCHMARK_ENTRY(Name, Record)
#endif

STATIC_ARENA_BENCHMARK_ENTRY(LLVMStaticArenaBenchmarkZeroFilled,
                             ZeroFilledRecord);
STATIC_ARENA_BENCHMARK_ENTRY(LLVMStaticArenaBenchmarkTemplate, TemplateRecord);

#undef STATIC_ARENA_BENCHMARK_ENTRY
#if defined(_MSC_VER)
#undef STATIC_ARENA_BENCHMARK_FORCE_INCLUDE
#endif

namespace {

struct InvocationMeasurement {
  StaticArenaCreateStats Create;
  uint64_t LifetimeNanoseconds = 0;
};

static InvocationMeasurement measureInvocation() {
  InvocationMeasurement Measurement;
  auto Start = std::chrono::steady_clock::now();
  std::unique_ptr<StaticArena> Arena = StaticArena::create(Measurement.Create);
  {
    ScopedStaticArenaBinding Binding(*Arena);
    Arena->beginClosing();
    Arena->runDestructors();
  }
  Arena.reset();
  Measurement.LifetimeNanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - Start)
          .count();
  return Measurement;
}

static void addProfileCounters(benchmark::State &State,
                               const InvocationMeasurement &Cold,
                               const InvocationMeasurement &Warm) {
  auto Set = [&](const char *Name, uint64_t Value) {
    State.counters[Name] = static_cast<double>(Value);
  };
  Set("cold_lifetime_ns", Cold.LifetimeNanoseconds);
  Set("cold_create_ns", Cold.Create.TotalNanoseconds);
  Set("cold_layout_ns", Cold.Create.LayoutNanoseconds);
  Set("warm_lifetime_ns", Warm.LifetimeNanoseconds);
  Set("warm_create_ns", Warm.Create.TotalNanoseconds);
  Set("warm_layout_ns", Warm.Create.LayoutNanoseconds);
  Set("warm_allocate_ns", Warm.Create.AllocationNanoseconds);
  Set("warm_zero_fill_ns", Warm.Create.ZeroFillNanoseconds);
  Set("warm_record_init_ns", Warm.Create.RecordInitializationNanoseconds);
  Set("records", Warm.Create.RecordCount);
  Set("template_bytes", Warm.Create.TemplateBytes);
  Set("object_bytes", Warm.Create.ObjectBytes);
  Set("allocation_bytes", Warm.Create.AllocationBytes);
  Set("backing_bytes", Warm.Create.BackingBytes);
  Set("used_demand_zero_mapping", Warm.Create.UsedDemandZeroMapping);
}

static void BM_StaticArenaInvocation(benchmark::State &State) {
#if !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  State.SkipWithError("the v1 producer supports COFF, ELF, and Wasm only");
#else
  // Capture the process-cold layout cost and the immediately following warm
  // creation separately. The benchmark loop then measures the recurring full
  // invocation lifetime without the opt-in clocks used by create(Stats).
  struct ColdWarmMeasurements {
    InvocationMeasurement Cold = measureInvocation();
    InvocationMeasurement Warm = measureInvocation();
  };
  static const ColdWarmMeasurements Measurements;
  addProfileCounters(State, Measurements.Cold, Measurements.Warm);

  for (auto _ : State) {
    std::unique_ptr<StaticArena> Arena = StaticArena::create();
    {
      ScopedStaticArenaBinding Binding(*Arena);
      benchmark::DoNotOptimize(Arena.get());
      Arena->beginClosing();
      Arena->runDestructors();
    }
    Arena.reset();
  }
#endif
}

BENCHMARK(BM_StaticArenaInvocation)->Unit(benchmark::kMicrosecond);

} // namespace

BENCHMARK_MAIN();
