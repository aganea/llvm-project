// Verify that the production arena list selects CommandFlags' source-qualified
// pointer caches without capturing every unrelated symbol named "*View".
//
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm \
// RUN:   -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%S/../../../llvm/cmake/modules/llvm-static-arena-list.txt \
// RUN:   -o - %s | FileCheck %s --check-prefixes=CHECK,ELF \
// RUN:   --implicit-check-not=__llvm_arena_var_v1.UnrelatedView \
// RUN:   --implicit-check-not=__llvm_arena_var_v1._ZN4llvm19SomeDebugFlagSuffixE \
// RUN:   --implicit-check-not=AlmostLCDylib \
// RUN:   --implicit-check-not=pageInQueueSuffix
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -std=c++17 -emit-llvm \
// RUN:   -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%S/../../../llvm/cmake/modules/llvm-static-arena-list.txt \
// RUN:   -o - %s | FileCheck %s --check-prefixes=CHECK,COFF \
// RUN:   --implicit-check-not=__llvm_arena_var_v1.UnrelatedView \
// RUN:   --implicit-check-not=__llvm_arena_var_v1.?SomeDebugFlagSuffix@llvm@@ \
// RUN:   --implicit-check-not=AlmostLCDylib \
// RUN:   --implicit-check-not=pageInQueueSuffix

#include "Inputs/CommandFlags.cpp"

namespace llvm {
bool DebugFlag;
bool SomeDebugFlagSuffix;

struct SPIRVExtensionsParser {
  static int DisabledExtensions;
};
int SPIRVExtensionsParser::DisabledExtensions;

namespace sampleprof {
struct FunctionSamples {
  static int ProfileIsProbeBased;
  static int ProfileIsCS;
  static int ProfileIsPreInlined;
  static int UseMD5;
  static int HasUniqSuffix;
  static int ProfileIsFS;
};
int FunctionSamples::ProfileIsProbeBased;
int FunctionSamples::ProfileIsCS;
int FunctionSamples::ProfileIsPreInlined;
int FunctionSamples::UseMD5;
int FunctionSamples::HasUniqSuffix;
int FunctionSamples::ProfileIsFS;
} // namespace sampleprof

struct MipsSubtarget {
  static int DspWarningPrinted;
  static int MSAWarningPrinted;
  static int VirtWarningPrinted;
  static int CRCWarningPrinted;
  static int GINVWarningPrinted;
};
int MipsSubtarget::DspWarningPrinted;
int MipsSubtarget::MSAWarningPrinted;
int MipsSubtarget::VirtWarningPrinted;
int MipsSubtarget::CRCWarningPrinted;
int MipsSubtarget::GINVWarningPrinted;

// TrackingStatistic has a constexpr initializer but contains atomics whose
// copy constructors are deleted. Model that relevant representation property
// here so the production type selector cannot regress into rejecting every
// STATISTIC definition as a non-cloneable constant object.
struct TrackingAtomic {
  unsigned long long Value;

  constexpr TrackingAtomic(unsigned long long Value) : Value(Value) {}
  TrackingAtomic(const TrackingAtomic &) = delete;
};

static_assert(__is_trivially_copyable(TrackingAtomic));
static_assert(__is_bitwise_cloneable(TrackingAtomic));

struct TrackingStatistic {
  const char *const DebugType;
  const char *const Name;
  const char *const Desc;
  TrackingAtomic Value;
  TrackingAtomic Initialized;

  constexpr TrackingStatistic(const char *DebugType, const char *Name,
                              const char *Desc)
      : DebugType(DebugType), Name(Name), Desc(Desc), Value(0), Initialized(0) {}
};

TrackingStatistic TrackedStat("arena", "TrackedStat", "tracked statistic");

struct TimerGroup {
  int Value;
};
TimerGroup SupportTimerGroup;

struct ExitOnError {
  int Value;
};
ExitOnError ToolExitHandler;

struct ThreadPoolStrategy {
  unsigned ThreadsRequested;
};
namespace parallel {
ThreadPoolStrategy strategy;
} // namespace parallel
} // namespace llvm

namespace {
int InvocationFatalErrorHandlerState;
}

namespace clang::ento {
struct CounterEntryPointTranslationUnitStat {
  int Value;
};
struct UnsignedMaxEntryPointTranslationUnitStatistic {
  int Value;
};
struct UnsignedEPStat {
  int Value;
};

CounterEntryPointTranslationUnitStat EntryPointCounter;
UnsignedMaxEntryPointTranslationUnitStatistic EntryPointMaximum;
UnsignedEPStat EntryPointUnsigned;
} // namespace clang::ento

namespace lld::wasm {
int ctx;
int symtab;
int tar;
int out;
struct BitcodeFile {
  static int doneLTO;
};
int BitcodeFile::doneLTO;

int ctxSuffix;
} // namespace lld::wasm

namespace lld::macho {
int config;
int inputFiles;
int priorityBuilder;
struct InputFile {
  static int idCount;
};
int InputFile::idCount;

int configuration;
} // namespace lld::macho

namespace selection_test {
struct LCDylib {
  static int instanceCount;
};
int LCDylib::instanceCount;

struct AlmostLCDylib {
  static int instanceCount;
};
int AlmostLCDylib::instanceCount;
} // namespace selection_test

int pageInQueue;
int pageInQueueSuffix;

namespace llvm::cl {
struct extrahelp {
  int Value;
};
extrahelp ExtraHelp;
} // namespace llvm::cl

struct TestGICombinerRuleOptionStorage {
  int Value;
};
TestGICombinerRuleOptionStorage GeneratedCombinerOptions;
int SrcAddrSpaces;

int &getRunPassNamesForTest() {
  static int RunPassNames;
  return RunPassNames;
}

namespace polly {
int PollyNumThreads;
int PollyScheduling;
int PollyChunkSize;
int PerfMonitoring;
int UseInstructionNames;
int ViewFilter;
int ViewAll;
int PollyProcessUnprofitable;
int PollyAllowFullFunction;
int PollyAllowUnsignedOperations;
int PollyUseRuntimeAliasChecks;
int PollyTrackFailures;
int PollyDelinearize;
int PollyInvariantLoadHoisting;
int ModelReadOnlyScalars;
int OptAnalysisLevel;
int PollyVectorizerChoice;
} // namespace polly

int PollyDebugPrinting;
int TraceStmts;
int PollyDebugFlag;

extern "C" int UnrelatedView;
int UnrelatedView;

int useViews() {
  return (llvm::codegen::CodeGenView != nullptr) +
         (llvm::mc::MCView != nullptr) + UnrelatedView;
}

int useDebugFlags() {
  return llvm::DebugFlag + llvm::SomeDebugFlagSuffix;
}

unsigned long long useTrackingStatistic() {
  return llvm::TrackedStat.Value.Value;
}

int useTimerGroup() { return llvm::SupportTimerGroup.Value; }
int useExitHandler() { return llvm::ToolExitHandler.Value; }
unsigned useParallelStrategy() {
  return llvm::parallel::strategy.ThreadsRequested;
}
int useFatalErrorHandlerState() { return InvocationFatalErrorHandlerState; }

int useEntryPointStatistics() {
  return clang::ento::EntryPointCounter.Value +
         clang::ento::EntryPointMaximum.Value +
         clang::ento::EntryPointUnsigned.Value;
}

int useCallbackClusters() {
  return llvm::cl::ExtraHelp.Value + GeneratedCombinerOptions.Value +
         SrcAddrSpaces;
}

int useRunPassNames() { return getRunPassNamesForTest(); }

int usePollyLocations() {
  return polly::PollyNumThreads + polly::PollyScheduling +
         polly::PollyChunkSize + polly::PerfMonitoring +
         PollyDebugPrinting + TraceStmts + polly::UseInstructionNames +
         polly::ViewFilter + polly::ViewAll +
         polly::PollyProcessUnprofitable + polly::PollyAllowFullFunction +
         polly::PollyAllowUnsignedOperations +
         polly::PollyUseRuntimeAliasChecks + polly::PollyTrackFailures +
         polly::PollyDelinearize + polly::PollyInvariantLoadHoisting +
         polly::ModelReadOnlyScalars + polly::OptAnalysisLevel +
         polly::PollyVectorizerChoice + PollyDebugFlag;
}

// CHECK-DAG: @UnrelatedView = {{.*}}global i32 0
// ELF-DAG: @__llvm_arena_var_v1._ZN4llvm7codegen11CodeGenViewE =
// ELF-DAG: @__llvm_arena_var_v1._ZN4llvm2mc6MCViewE =
// ELF-DAG: @__llvm_arena_var_v1._ZN4llvm9DebugFlagE =
// ELF-DAG: @_ZN4llvm19SomeDebugFlagSuffixE = {{.*}}global i8 0
// COFF-DAG: @"__llvm_arena_var_v1.?CodeGenView@codegen@llvm@@
// COFF-DAG: @"__llvm_arena_var_v1.?MCView@mc@llvm@@
// COFF-DAG: @"__llvm_arena_var_v1.?DebugFlag@llvm@@
// COFF-DAG: @"?SomeDebugFlagSuffix@llvm@@{{.*}}" = {{.*}}global i8 0
// ELF-DAG: @__llvm_arena_var_v1._ZN4llvm11TrackedStatE =
// COFF-DAG: @"__llvm_arena_var_v1.?TrackedStat@llvm@@
// ELF-DAG: @__llvm_arena_var_v1._ZN4llvm17SupportTimerGroupE =
// COFF-DAG: @"__llvm_arena_var_v1.?SupportTimerGroup@llvm@@
// ELF-DAG: @__llvm_arena_var_v1._ZN4llvm8parallel8strategyE =
// COFF-DAG: @"__llvm_arena_var_v1.?strategy@parallel@llvm@@
// ELF-DAG: @__llvm_arena_var_v1._ZN12_GLOBAL__N_132InvocationFatalErrorHandlerStateE =
// COFF-DAG: @"__llvm_arena_var_v1.?InvocationFatalErrorHandlerState@?A0x{{[0-9A-F]+}}@@
// ELF-DAG: @__llvm_arena_var_v1._ZN5clang4ento17EntryPointCounterE =
// ELF-DAG: @__llvm_arena_var_v1._ZN5clang4ento17EntryPointMaximumE =
// ELF-DAG: @__llvm_arena_var_v1._ZN5clang4ento18EntryPointUnsignedE =
// COFF-DAG: @"__llvm_arena_var_v1.?EntryPointCounter@ento@clang@@
// COFF-DAG: @"__llvm_arena_var_v1.?EntryPointMaximum@ento@clang@@
// COFF-DAG: @"__llvm_arena_var_v1.?EntryPointUnsigned@ento@clang@@
// ELF-DAG: @__llvm_arena_var_v1._ZN4llvm21SPIRVExtensionsParser18DisabledExtensionsE =
// ELF-DAG: @__llvm_arena_var_v1._ZN4llvm10sampleprof15FunctionSamples19ProfileIsProbeBasedE =
// ELF-DAG: @__llvm_arena_var_v1._ZN4llvm13MipsSubtarget17DspWarningPrintedE =
// COFF-DAG: @"__llvm_arena_var_v1.?DisabledExtensions@SPIRVExtensionsParser@llvm@@
// COFF-DAG: @"__llvm_arena_var_v1.?ProfileIsProbeBased@FunctionSamples@sampleprof@llvm@@
// COFF-DAG: @"__llvm_arena_var_v1.?DspWarningPrinted@MipsSubtarget@llvm@@
// ELF-DAG: @__llvm_arena_var_v1._ZN3lld4wasm3ctxE =
// ELF-DAG: @__llvm_arena_var_v1._ZN3lld4wasm11BitcodeFile7doneLTOE =
// COFF-DAG: @"__llvm_arena_var_v1.?ctx@wasm@lld@@
// COFF-DAG: @"__llvm_arena_var_v1.?doneLTO@BitcodeFile@wasm@lld@@
// ELF-DAG: @__llvm_arena_var_v1._ZN3lld5macho6configE =
// ELF-DAG: @__llvm_arena_var_v1._ZN3lld5macho9InputFile7idCountE =
// COFF-DAG: @"__llvm_arena_var_v1.?config@macho@lld@@
// COFF-DAG: @"__llvm_arena_var_v1.?idCount@InputFile@macho@lld@@
// ELF-DAG: @__llvm_arena_var_v1._ZN14selection_test7LCDylib13instanceCountE =
// COFF-DAG: @"__llvm_arena_var_v1.?instanceCount@LCDylib@selection_test@@
// ELF-DAG: @__llvm_arena_var_v1.pageInQueue =
// COFF-DAG: @"__llvm_arena_var_v1.?pageInQueue@@
// CHECK-LABEL: define {{.*}}getRunPassNamesForTest
// CHECK: call ptr @__llvm_arena_addr_v1
// ELF-LABEL: define {{.*}}i32 @_Z8useViewsv()
// ELF: call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1._ZN4llvm7codegen11CodeGenViewE)
// ELF: call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1._ZN4llvm2mc6MCViewE)
// COFF-LABEL: define {{.*}}i32 @"?useViews@@YAHXZ"()
// COFF: call ptr @__llvm_arena_addr_v1(ptr @"__llvm_arena_var_v1.?CodeGenView@codegen@llvm@@
// COFF: call ptr @__llvm_arena_addr_v1(ptr @"__llvm_arena_var_v1.?MCView@mc@llvm@@
// CHECK: load i32, ptr @UnrelatedView
// ELF-LABEL: define {{.*}}i32 @_Z13useDebugFlagsv()
// ELF: call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1._ZN4llvm9DebugFlagE)
// ELF: load i8, ptr @_ZN4llvm19SomeDebugFlagSuffixE
// COFF-LABEL: define {{.*}}i32 @"?useDebugFlags@@YAHXZ"()
// COFF: call ptr @__llvm_arena_addr_v1(ptr @"__llvm_arena_var_v1.?DebugFlag@llvm@@
// COFF: load i8, ptr @"?SomeDebugFlagSuffix@llvm@@
// ELF-LABEL: define {{.*}}i64 @_Z20useTrackingStatisticv()
// ELF: call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1._ZN4llvm11TrackedStatE)
// COFF-LABEL: define {{.*}}i64 @"?useTrackingStatistic@@YA_KXZ"()
// COFF: call ptr @__llvm_arena_addr_v1(ptr @"__llvm_arena_var_v1.?TrackedStat@llvm@@
// CHECK-LABEL: define {{.*}}useTimerGroup
// CHECK: call ptr @__llvm_arena_addr_v1
// CHECK-LABEL: define {{.*}}useExitHandler
// CHECK: call ptr @__llvm_arena_addr_v1
// CHECK-LABEL: define {{.*}}useParallelStrategy
// CHECK: call ptr @__llvm_arena_addr_v1
// CHECK-LABEL: define {{.*}}useFatalErrorHandlerState
// CHECK: call ptr @__llvm_arena_addr_v1
// CHECK-LABEL: define {{.*}}useCallbackClusters
// CHECK-COUNT-3: call ptr @__llvm_arena_addr_v1
// CHECK-LABEL: define {{.*}}usePollyLocations
// CHECK-COUNT-20: call ptr @__llvm_arena_addr_v1
