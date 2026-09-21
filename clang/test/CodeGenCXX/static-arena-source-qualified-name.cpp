// Verify that source-qualified name selectors distinguish identical
// file-local names without relying on target mangling.
//
// RUN: split-file %s %t
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   %t/chosen.cpp -o - | FileCheck %s --check-prefix=CHOSEN
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   %t/other.cpp -o - | FileCheck %s --check-prefix=OTHER \
// RUN:   --implicit-check-not=__llvm_arena_
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm \
// RUN:   -fstatic-arena=arena_test -fstatic-arena-list=%t/arena.list \
// RUN:   %t/qualified.cpp -o - | FileCheck %s --check-prefix=QUALIFIED
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm \
// RUN:   -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%S/../../../llvm/cmake/modules/llvm-static-arena-list.txt \
// RUN:   %t/llvm-objdump.cpp -o - | FileCheck %s --check-prefix=PRODUCTION
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -x c++ -emit-llvm \
// RUN:   -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%S/../../../llvm/cmake/modules/llvm-static-arena-list.txt \
// RUN:   %t/Process.inc -o - | FileCheck %s --check-prefix=PROCESS-POLICY
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm \
// RUN:   -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%S/../../../llvm/cmake/modules/llvm-static-arena-list.txt \
// RUN:   %t/SmartPtrChecker.cpp -o - | FileCheck %s --check-prefix=SMARTPTR
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm \
// RUN:   -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%S/../../../llvm/cmake/modules/llvm-static-arena-list.txt \
// RUN:   %t/DeclBase.cpp -o - | FileCheck %s --check-prefix=AST-DECL
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm \
// RUN:   -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%S/../../../llvm/cmake/modules/llvm-static-arena-list.txt \
// RUN:   %t/Stmt.cpp -o - | FileCheck %s --check-prefix=AST-STMT
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm \
// RUN:   -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%S/../../../llvm/cmake/modules/llvm-static-arena-list.txt \
// RUN:   %t/ARM64.cpp -o - | FileCheck %s --check-prefix=MACHO-TARGET
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm \
// RUN:   -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%S/../../../llvm/cmake/modules/llvm-static-arena-list.txt \
// RUN:   %t/ARM64_32.cpp -o - | FileCheck %s --check-prefix=MACHO-TARGET
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm \
// RUN:   -fstatic-arena=arena_test \
// RUN:   -fstatic-arena-list=%S/../../../llvm/cmake/modules/llvm-static-arena-list.txt \
// RUN:   %t/X86_64.cpp -o - | FileCheck %s --check-prefix=MACHO-TARGET

//--- arena.list
[arena]
name:*chosen.cpp@State
name:*qualified.cpp@selected::Value

//--- chosen.cpp
static int State;
static int Unselected;
int chosen() { return State + Unselected; }

// CHOSEN: @__llvm_arena_var_v1._ZL5State =
// CHOSEN: @_ZL10Unselected = internal global i32 0

//--- other.cpp
static int State;
int other() { return State; }

// OTHER: @_ZL5State = internal global i32 0

//--- qualified.cpp
namespace selected {
int Value;
}
namespace other {
int Value;
}
int qualified() { return selected::Value + other::Value; }

// QUALIFIED: @__llvm_arena_var_v1._ZN8selected5ValueE =
// QUALIFIED: @_ZN5other5ValueE = global i32 0

//--- Process.inc
struct AtomicBool {
  bool Value;
  constexpr AtomicBool(bool Value) : Value(Value) {}
  AtomicBool(const AtomicBool &) = delete;
};
static_assert(__is_trivially_copyable(AtomicBool));
static_assert(__is_bitwise_cloneable(AtomicBool));

static AtomicBool UseANSI(false);
static AtomicBool UseANSISuffix(false);
bool processPolicy() { return UseANSI.Value || UseANSISuffix.Value; }

// PROCESS-POLICY: @__llvm_arena_var_v1._ZL7UseANSI =
// PROCESS-POLICY: @_ZL13UseANSISuffix = internal global

//--- llvm-objdump.cpp
static int AdjustVMA;
static int HexagonPrettyPrinterInst;
namespace llvm::objdump {
int ArchiveHeaders;
}
int useObjdumpState() {
  return AdjustVMA + HexagonPrettyPrinterInst + llvm::objdump::ArchiveHeaders;
}

// PRODUCTION-DAG: @__llvm_arena_var_v1._ZL9AdjustVMA =
// PRODUCTION-DAG: @__llvm_arena_var_v1._ZL24HexagonPrettyPrinterInst =
// PRODUCTION-DAG: @__llvm_arena_var_v1._ZN4llvm7objdump14ArchiveHeadersE =

//--- SmartPtrChecker.cpp
static const void *NullDereferenceBugTypePtr;
const void *getBugType() { return NullDereferenceBugTypePtr; }

// SMARTPTR: @__llvm_arena_var_v1._ZL25NullDereferenceBugTypePtr =

//--- DeclBase.cpp
namespace clang {
struct Decl {
  static bool StatisticsEnabled;
};
bool Decl::StatisticsEnabled;
} // namespace clang
#define DECL(DERIVED, BASE) static int n##DERIVED##s;
#include "DeclNodes.inc"
#undef DECL
static int OtherCounter;
int declStats() {
  return clang::Decl::StatisticsEnabled + nFunctionDecls + nVarDecls +
         OtherCounter;
}

// AST-DECL-DAG: @__llvm_arena_var_v1._ZN5clang4Decl17StatisticsEnabledE =
// AST-DECL-DAG: @__llvm_arena_var_v1._ZL14nFunctionDecls =
// AST-DECL-DAG: @__llvm_arena_var_v1._ZL9nVarDecls =
// AST-DECL-DAG: @_ZL12OtherCounter = internal global i32 0

//--- DeclNodes.inc
DECL(FunctionDecl, Decl)
DECL(VarDecl, Decl)

//--- Stmt.cpp
namespace clang {
struct Stmt {
  static bool StatisticsEnabled;
};
bool Stmt::StatisticsEnabled;
} // namespace clang
int &getStmtInfoEntry() {
  static int StmtClassInfo;
  return StmtClassInfo;
}
static int OtherStmtCounter;
int stmtStats() {
  return clang::Stmt::StatisticsEnabled + getStmtInfoEntry() +
         OtherStmtCounter;
}

// AST-STMT-DAG: @__llvm_arena_var_v1._ZN5clang4Stmt17StatisticsEnabledE =
// AST-STMT-DAG: @__llvm_arena_var_v1.{{.*}}StmtClassInfo =
// AST-STMT-DAG: @_ZL16OtherStmtCounter = internal global i32 0

//--- ARM64.cpp
struct TargetInfo {
  virtual ~TargetInfo();
  unsigned long long pageZeroSize = 1ULL << 32;
};
struct ARM64 : TargetInfo {};
TargetInfo *createARM64TargetInfo() {
  static ARM64 t;
  return &t;
}

//--- ARM64_32.cpp
struct TargetInfo {
  virtual ~TargetInfo();
  unsigned long long pageZeroSize = 1ULL << 12;
};
struct ARM64_32 : TargetInfo {};
TargetInfo *createARM64_32TargetInfo() {
  static ARM64_32 t;
  return &t;
}

//--- X86_64.cpp
struct TargetInfo {
  virtual ~TargetInfo();
  unsigned long long pageZeroSize = 1ULL << 32;
};
struct X86_64 : TargetInfo {};
TargetInfo *createX86_64TargetInfo() {
  static X86_64 t;
  return &t;
}

// The polymorphic target and its initialization guard must both be relocated
// into per-invocation storage. The exact internal names differ by source file,
// so intentionally match their common suffixes here.
// MACHO-TARGET-DAG: @__llvm_arena_var_v1.{{.*}}t =
// MACHO-TARGET-DAG: @__llvm_arena_var_v1._ZGVZ{{.*}}t =
// MACHO-TARGET: call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1._ZZ{{.*}}t)
// MACHO-TARGET: call ptr @__llvm_arena_addr_v1(ptr @__llvm_arena_var_v1._ZGVZ{{.*}}t)
