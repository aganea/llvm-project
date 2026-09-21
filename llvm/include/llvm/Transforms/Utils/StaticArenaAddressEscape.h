//===- StaticArenaAddressEscape.h - Check arena address stores -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_STATICARENAADDRESSESCAPE_H
#define LLVM_TRANSFORMS_UTILS_STATICARENAADDRESSESCAPE_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

class CallBase;
class GlobalValue;
class Module;
class StoreInst;

/// One store of a value derived from a static-arena resolver call into
/// process-global storage.
struct StaticArenaAddressEscape {
  StoreInst *Store;
  CallBase *ResolverCall;
  /// Null when bounded destination analysis was exhausted before finding a
  /// specific process-global destination.
  const GlobalValue *Destination;

  /// The storage name encoded by the resolver's v1 record operand.
  StringRef getArenaStorageName() const;

  /// The process-global destination, or "<unknown>" when analysis exhausted
  /// its budget before finding one.
  StringRef getDestinationName() const;
};

/// Find the deliberately narrow cached-address escape class checked by the
/// static-arena producer. Values are followed through casts, GEPs and
/// phi/select nodes. A global destination is safe only when all of its SSA
/// roots are resolver results or the global carries the "static-arena"
/// attribute. Calls, returns and arbitrary memory dataflow are not a proof
/// target for this checker.
LLVM_ABI SmallVector<StaticArenaAddressEscape>
findStaticArenaAddressEscapes(Module &M,
                              StringRef ResolverName = "__llvm_arena_addr_v1");

/// An independently runnable verifier wrapper around
/// findStaticArenaAddressEscapes. The pass does not mutate IR.
class StaticArenaAddressEscapePass
    : public RequiredPassInfoMixin<StaticArenaAddressEscapePass> {
public:
  LLVM_ABI PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_STATICARENAADDRESSESCAPE_H
