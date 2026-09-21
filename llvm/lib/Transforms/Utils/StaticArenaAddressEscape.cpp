//===- StaticArenaAddressEscape.cpp - Check arena address stores ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/StaticArenaAddressEscape.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

using namespace llvm;

namespace {

constexpr StringLiteral StaticArenaAttribute = "static-arena";
constexpr StringLiteral StaticArenaRecordPrefix = "__llvm_arena_var_v1.";

// This verifier is mandatory for every static-arena translation unit, so keep
// adversarial SSA from consuming unbounded time or stack. Exceeding either
// budget is treated conservatively by the caller.
constexpr unsigned MaxArenaAddressValues = 4096;
constexpr unsigned MaxArenaAddressEdges = 16384;

struct ArenaAddressRootSearch {
  CallBase *Root = nullptr;
  bool Proven = false;
  bool Exhausted = false;
};

struct ArenaGlobalSearch {
  const GlobalValue *Destination = nullptr;
  bool Exhausted = false;
};

static bool isStaticArenaGlobal(const GlobalValue *Global) {
  const auto *GV = dyn_cast<GlobalVariable>(Global);
  return GV && GV->hasAttribute(StaticArenaAttribute);
}

static CallBase *getArenaResolverCall(Value *V, StringRef ResolverName) {
  auto *Call = dyn_cast<CallBase>(V);
  if (!Call)
    return nullptr;
  Function *Callee = Call->getCalledFunction();
  return Callee && Callee->getName() == ResolverName ? Call : nullptr;
}

static unsigned getArenaPathOperandCount(Value *V) {
  unsigned Opcode = Operator::getOpcode(V);
  if (Instruction::isCast(Opcode) || Opcode == Instruction::GetElementPtr)
    return 1;
  if (auto *Phi = dyn_cast<PHINode>(V))
    return Phi->getNumIncomingValues();
  if (Opcode == Instruction::Select)
    return 2;
  return 0;
}

static Value *getArenaPathOperand(Value *V, unsigned Index) {
  unsigned Opcode = Operator::getOpcode(V);
  if (Instruction::isCast(Opcode))
    return cast<Operator>(V)->getOperand(0);
  if (auto *GEP = dyn_cast<GEPOperator>(V))
    return GEP->getPointerOperand();
  if (auto *Phi = dyn_cast<PHINode>(V))
    return Phi->getIncomingValue(Index);
  assert(Opcode == Instruction::Select && "unsupported arena path operator");
  return cast<Operator>(V)->getOperand(Index + 1);
}

// Search existentially for an arena root. If the budget is exhausted before a
// root is found, the caller diagnoses the store with an unknown arena source
// rather than silently accepting it.
static ArenaAddressRootSearch findAnyArenaAddressRoot(Value *V,
                                                      StringRef ResolverName) {
  SmallVector<Value *, 32> Worklist;
  SmallPtrSet<Value *, 32> Visited;
  Worklist.push_back(V);
  unsigned EdgeCount = 0;

  while (!Worklist.empty()) {
    Value *Current = Worklist.pop_back_val();
    if (!Visited.insert(Current).second)
      continue;
    if (Visited.size() > MaxArenaAddressValues)
      return {nullptr, false, true};

    if (CallBase *Root = getArenaResolverCall(Current, ResolverName))
      return {Root, true, false};

    unsigned OperandCount = getArenaPathOperandCount(Current);
    if (OperandCount > MaxArenaAddressEdges - EdgeCount)
      return {nullptr, false, true};
    EdgeCount += OperandCount;
    for (unsigned I = 0; I != OperandCount; ++I)
      Worklist.push_back(getArenaPathOperand(Current, I));
  }
  return {nullptr, true, false};
}

// Search for a process-global destination without falling back to the
// unbounded underlying-object walk. If an unexplored path remains when either
// budget is exhausted, the caller diagnoses an unknown destination.
static ArenaGlobalSearch findAnyNonArenaGlobal(Value *V) {
  SmallVector<Value *, 32> Worklist;
  SmallPtrSet<Value *, 32> Visited;
  Worklist.push_back(V);
  unsigned EdgeCount = 0;

  while (!Worklist.empty()) {
    Value *Current = Worklist.pop_back_val();
    if (!Visited.insert(Current).second)
      continue;
    if (Visited.size() > MaxArenaAddressValues)
      return {nullptr, true};

    if (auto *Global = dyn_cast<GlobalValue>(Current)) {
      if (!isStaticArenaGlobal(Global))
        return {Global, false};
      continue;
    }

    unsigned OperandCount = getArenaPathOperandCount(Current);
    if (OperandCount > MaxArenaAddressEdges - EdgeCount)
      return {nullptr, true};
    EdgeCount += OperandCount;
    for (unsigned I = 0; I != OperandCount; ++I)
      Worklist.push_back(getArenaPathOperand(Current, I));
  }
  return {};
}

// Prove universally that every path through the supported SSA operators ends
// at an arena resolver. The explicit DFS avoids recursion and detects cycles;
// cycles, unknown leaves and budget exhaustion all fail the proof.
static ArenaAddressRootSearch
proveEveryArenaAddressRoot(Value *V, StringRef ResolverName) {
  enum class VisitState : uint8_t { Visiting, Proven };
  struct Frame {
    Value *Current;
    unsigned NextOperand = 0;
  };

  SmallDenseMap<Value *, VisitState, 32> States;
  SmallVector<Frame, 32> Stack;
  States.try_emplace(V, VisitState::Visiting);
  Stack.push_back({V});
  unsigned EdgeCount = 0;
  CallBase *RepresentativeRoot = nullptr;

  while (!Stack.empty()) {
    Frame &CurrentFrame = Stack.back();
    Value *Current = CurrentFrame.Current;

    if (CallBase *Root = getArenaResolverCall(Current, ResolverName)) {
      if (!RepresentativeRoot)
        RepresentativeRoot = Root;
      States.find(Current)->second = VisitState::Proven;
      Stack.pop_back();
      continue;
    }

    unsigned OperandCount = getArenaPathOperandCount(Current);
    if (OperandCount == 0)
      return {};
    if (CurrentFrame.NextOperand == OperandCount) {
      States.find(Current)->second = VisitState::Proven;
      Stack.pop_back();
      continue;
    }

    if (EdgeCount == MaxArenaAddressEdges)
      return {RepresentativeRoot, false, true};
    ++EdgeCount;
    Value *Operand = getArenaPathOperand(Current, CurrentFrame.NextOperand++);
    auto Existing = States.find(Operand);
    if (Existing != States.end()) {
      if (Existing->second == VisitState::Visiting)
        return {RepresentativeRoot, false, false};
      continue;
    }
    if (States.size() == MaxArenaAddressValues)
      return {RepresentativeRoot, false, true};
    States.try_emplace(Operand, VisitState::Visiting);
    Stack.push_back({Operand});
  }

  return {RepresentativeRoot, RepresentativeRoot != nullptr, false};
}

} // end anonymous namespace

StringRef StaticArenaAddressEscape::getArenaStorageName() const {
  if (!ResolverCall || ResolverCall->arg_empty())
    return "<unknown>";
  Value *Arg = ResolverCall->getArgOperand(0)->stripPointerCasts();
  auto *Record = dyn_cast<GlobalVariable>(Arg);
  if (!Record)
    return "<unknown>";
  StringRef Name = Record->getName();
  return Name.starts_with(StaticArenaRecordPrefix)
             ? Name.drop_front(StaticArenaRecordPrefix.size())
             : Name;
}

StringRef StaticArenaAddressEscape::getDestinationName() const {
  return Destination ? Destination->getName() : "<unknown>";
}

SmallVector<StaticArenaAddressEscape>
llvm::findStaticArenaAddressEscapes(Module &M, StringRef ResolverName) {
  SmallVector<StaticArenaAddressEscape> Escapes;
  for (Function &F : M) {
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *Store = dyn_cast<StoreInst>(&I);
        if (!Store)
          continue;
        ArenaAddressRootSearch Source =
            findAnyArenaAddressRoot(Store->getValueOperand(), ResolverName);
        if (!Source.Root && !Source.Exhausted)
          continue;
        ArenaAddressRootSearch DestinationSearch = proveEveryArenaAddressRoot(
            Store->getPointerOperand(), ResolverName);
        if (DestinationSearch.Proven)
          continue;
        if (DestinationSearch.Exhausted) {
          Escapes.push_back({Store, Source.Root, nullptr});
          continue;
        }

        ArenaGlobalSearch GlobalSearch =
            findAnyNonArenaGlobal(Store->getPointerOperand());
        if (GlobalSearch.Destination)
          Escapes.push_back({Store, Source.Root, GlobalSearch.Destination});
        else if (GlobalSearch.Exhausted)
          Escapes.push_back({Store, Source.Root, nullptr});
      }
    }
  }
  return Escapes;
}

PreservedAnalyses StaticArenaAddressEscapePass::run(Module &M,
                                                    ModuleAnalysisManager &AM) {
  for (const StaticArenaAddressEscape &Escape :
       findStaticArenaAddressEscapes(M))
    M.getContext().emitError(
        Escape.Store,
        Twine("arena address of ") + Escape.getArenaStorageName() +
            " is stored to non-arena global " + Escape.getDestinationName());
  return PreservedAnalyses::all();
}
