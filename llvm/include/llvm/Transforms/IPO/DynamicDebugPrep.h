//===- DynamicDebugPrep.h - Prepare for dynamic debugging -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Two passes for dynamic debugging (on-demand deoptimization):
//
// DynamicDebugPrepPass (build time, -fdynamic-debug-prep):
//   Promotes internal-linkage functions and globals by creating external
//   aliases with a TU-unique ".dyndbg.<hash>" suffix.
//
// DynamicDebugExternPass (recompile time, -fdynamic-debug-extern):
//   Converts internal-linkage globals and functions into extern declarations
//   using the promoted ".dyndbg.<hash>" names.  The resulting -O0 .obj has
//   no duplicate definitions -- only extern relocations that the debugger
//   resolves against the PDB.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_IPO_DYNAMICDEBUGPREP_H
#define LLVM_TRANSFORMS_IPO_DYNAMICDEBUGPREP_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Module;

/// Build-time: create ".dyndbg.<hash>" aliases for internal-linkage symbols.
struct DynamicDebugPrepPass : PassInfoMixin<DynamicDebugPrepPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &);
};

/// Recompile-time: turn internal-linkage globals into extern declarations
/// using the promoted ".dyndbg.<hash>" names so the -O0 .obj references
/// the optimized binary's symbols directly.
struct DynamicDebugExternPass : PassInfoMixin<DynamicDebugExternPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_IPO_DYNAMICDEBUGPREP_H
