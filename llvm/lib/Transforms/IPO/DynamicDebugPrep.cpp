//===- DynamicDebugPrep.cpp - Prepare for dynamic debugging ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Two passes for dynamic debugging (on-demand deoptimization).
//
// DynamicDebugPrepPass (build time):
//   Promotes internal-linkage functions and globals by creating external
//   aliases with a TU-unique ".dyndbg.<hash>" suffix and adding them to
//   @llvm.used.  Functions keep internal linkage (the optimizer can still
//   inline them); only the alias is external.
//
// DynamicDebugExternPass (recompile time):
//   Renames internal-linkage functions and globals to their promoted
//   ".dyndbg.<hash>" names, makes them external declarations, and drops
//   their bodies/initializers.  The resulting -O0 .obj has only extern
//   relocations that the debugger resolves against the optimized binary's
//   PDB -- no duplicate definitions, no wasted .bss.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/DynamicDebugPrep.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

#define DEBUG_TYPE "dynamic-debug-prep"

static cl::list<std::string> ExternKeepFunctions(
    "dynamic-debug-extern-keep", cl::Hidden,
    cl::desc("Functions to keep (not externalize) in DynamicDebugExternPass. "
             "Matched as substring against the IR function name."));

static cl::opt<std::string> ExternTUHash(
    "dynamic-debug-extern-hash", cl::Hidden,
    cl::desc("Override TU hash for DynamicDebugExternPass (extracted from the "
             "optimized binary's PDB). When set, the extern pass uses this "
             "hash instead of recomputing it."));

static std::string computeTUHash(const Module &M) {
  // Hash only the source filename so the TU hash is stable across
  // different compilation modes (build-time -O3 vs recompile-time -O0).
  // getModuleIdentifier() can differ (e.g. different -o paths), so we
  // exclude it.
  MD5 Hash;
  Hash.update(M.getSourceFileName());
  MD5::MD5Result Result;
  Hash.final(Result);
  SmallString<16> Str;
  MD5::stringifyResult(Result, Str);
  return std::string(Str.substr(0, 16));
}

static bool isSpecialGlobal(const GlobalVariable &GV) {
  StringRef Name = GV.getName();
  if (Name.starts_with("llvm.") || Name.starts_with("__"))
    return true;
  if (GV.getSection().starts_with(".llvm"))
    return true;
  return false;
}

// ── DynamicDebugPrepPass (build time) ──────────────────────────────────────

PreservedAnalyses DynamicDebugPrepPass::run(Module &M,
                                            ModuleAnalysisManager &) {
  std::string TUHash = computeTUHash(M);
  SmallVector<GlobalValue *, 16> NewGlobals;
  bool Changed = false;

  for (Function &F : M) {
    if (F.isDeclaration() || !F.hasLocalLinkage() || F.isIntrinsic())
      continue;

    std::string AliasName = (F.getName() + ".dyndbg." + TUHash).str();
    auto *GA = GlobalAlias::create(F.getValueType(), F.getAddressSpace(),
                                   GlobalValue::ExternalLinkage, AliasName, &F,
                                   &M);
    GA->setVisibility(GlobalValue::HiddenVisibility);
    GA->setUnnamedAddr(GlobalValue::UnnamedAddr::None);
    NewGlobals.push_back(GA);
    LLVM_DEBUG(dbgs() << "DynamicDebugPrep: promoted function "
                      << F.getName() << " via alias " << AliasName << "\n");
    Changed = true;
  }

  for (GlobalVariable &GV : M.globals()) {
    if (GV.isDeclaration() || !GV.hasLocalLinkage())
      continue;
    if (isSpecialGlobal(GV))
      continue;

    std::string AliasName = (GV.getName() + ".dyndbg." + TUHash).str();
    auto *GA = GlobalAlias::create(GV.getValueType(), GV.getAddressSpace(),
                                   GlobalValue::ExternalLinkage, AliasName, &GV,
                                   &M);
    GA->setVisibility(GlobalValue::HiddenVisibility);
    GA->setUnnamedAddr(GlobalValue::UnnamedAddr::None);
    NewGlobals.push_back(GA);
    LLVM_DEBUG(dbgs() << "DynamicDebugPrep: promoted global "
                      << GV.getName() << " via alias " << AliasName << "\n");
    Changed = true;
  }

  if (!NewGlobals.empty())
    appendToUsed(M, NewGlobals);

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

// ── DynamicDebugExternPass (recompile time) ────────────────────────────────

PreservedAnalyses DynamicDebugExternPass::run(Module &M,
                                              ModuleAnalysisManager &) {
  std::string TUHash =
      ExternTUHash.empty() ? computeTUHash(M) : ExternTUHash.getValue();
  bool Changed = false;

  // Externalize internal-linkage globals: rename to the promoted name,
  // drop the initializer, set external linkage.  The -O0 .obj will emit
  // an IMAGE_SYM_UNDEFINED COFF symbol that the debugger resolves
  // against the PDB at load time.
  for (GlobalVariable &GV : make_early_inc_range(M.globals())) {
    if (GV.isDeclaration() || !GV.hasLocalLinkage())
      continue;
    if (isSpecialGlobal(GV))
      continue;

    std::string NewName = (GV.getName() + ".dyndbg." + TUHash).str();
    LLVM_DEBUG(dbgs() << "DynamicDebugExtern: externalized global "
                      << GV.getName() << " -> " << NewName << "\n");
    GV.setName(NewName);
    GV.setInitializer(nullptr);
    GV.setLinkage(GlobalValue::ExternalLinkage);
    GV.setVisibility(GlobalValue::DefaultVisibility);
    GV.setDSOLocal(false);
    Changed = true;
  }

  // Build a set of function names to keep (not externalize).
  auto shouldKeep = [](StringRef Name) {
    for (const auto &Keep : ExternKeepFunctions)
      if (Name.contains(Keep))
        return true;
    return false;
  };

  // Externalize internal-linkage functions: rename to the promoted name,
  // delete the body, set external linkage.  Calls from unoptimized code
  // will go through the PDB-resolved address (which points into the
  // optimized binary).
  for (Function &F : make_early_inc_range(M)) {
    if (F.isDeclaration() || !F.hasLocalLinkage() || F.isIntrinsic())
      continue;

    if (shouldKeep(F.getName())) {
      LLVM_DEBUG(dbgs() << "DynamicDebugExtern: keeping function "
                        << F.getName() << "\n");
      continue;
    }

    std::string NewName = (F.getName() + ".dyndbg." + TUHash).str();
    LLVM_DEBUG(dbgs() << "DynamicDebugExtern: externalized function "
                      << F.getName() << " -> " << NewName << "\n");
    F.setName(NewName);
    F.deleteBody();
    F.setLinkage(GlobalValue::ExternalLinkage);
    F.setVisibility(GlobalValue::DefaultVisibility);
    F.setDSOLocal(false);
    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
