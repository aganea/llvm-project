//===- llvm/CodeGen/MachinePassRegistry.h -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the mechanics for machine function pass registries.  A
// function pass registry (MachinePassRegistry) is auto filled by the static
// constructors of MachinePassRegistryNode.  Further there is a command line
// parser (RegisterPassParser) which listens to each registry for additions
// and deletions, so that the appropriate command option is updated.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_MACHINEPASSREGISTRY_H
#define LLVM_CODEGEN_MACHINEPASSREGISTRY_H

#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/ProcessWideRegistry.h"

namespace llvm {

//===----------------------------------------------------------------------===//
///
/// MachinePassRegistryListener - Listener to adds and removals of nodes in
/// registration list.
///
//===----------------------------------------------------------------------===//
template <class PassCtorTy> class MachinePassRegistryListener {
  virtual void anchor() {}

public:
  MachinePassRegistryListener() = default;
  virtual ~MachinePassRegistryListener() = default;

  virtual void NotifyAdd(StringRef N, PassCtorTy C, StringRef D) = 0;
  virtual void NotifyRemove(StringRef N) = 0;
};

//===----------------------------------------------------------------------===//
///
/// MachinePassRegistryNode - Machine pass node stored in registration list.
///
//===----------------------------------------------------------------------===//
template <typename PassCtorTy> class MachinePassRegistryNode {
private:
  MachinePassRegistryNode *Next = nullptr; // Next function pass in list.
  StringRef Name;                       // Name of function pass.
  StringRef Description;                // Description string.
  PassCtorTy Ctor;                      // Pass creator.

public:
  MachinePassRegistryNode(const char *N, const char *D, PassCtorTy C)
      : Name(N), Description(D), Ctor(C) {}

  // Accessors
  MachinePassRegistryNode *getNext()      const { return Next; }
  MachinePassRegistryNode **getNextAddress()    { return &Next; }
  StringRef getName()                   const { return Name; }
  StringRef getDescription()            const { return Description; }
  PassCtorTy getCtor() const { return Ctor; }
  void setNext(MachinePassRegistryNode *N)      { Next = N; }
};

//===----------------------------------------------------------------------===//
///
/// MachinePassRegistry - Track the registration of machine passes.
///
//===----------------------------------------------------------------------===//
template <typename PassCtorTy> class MachinePassRegistry {
private:
  MachinePassRegistryNode<PassCtorTy> *List = nullptr;

  struct InvocationState {
    PassCtorTy Default{}; // Default function pass creator.
    MachinePassRegistryListener<PassCtorTy> *Listener = nullptr;
  };

  // Registrations form a process-wide catalog. Selection and parser listener
  // state belong to the current tool invocation.
  ContextManagedStatic<InvocationState> State;

  InvocationState *getStateIfConstructed() {
    return State.isConstructed() ? &*State : nullptr;
  }

public:
  // NO CONSTRUCTOR - we don't want static constructor ordering to mess
  // with the registry.

  // Accessors.
  //
  MachinePassRegistryNode<PassCtorTy> *getList() { return List; }
  PassCtorTy getDefault() { return State->Default; }
  void setDefault(PassCtorTy C) { State->Default = C; }
  /// setDefault - Set the default constructor by name.
  void setDefault(StringRef Name) {
    PassCtorTy Ctor = nullptr;
    for (MachinePassRegistryNode<PassCtorTy> *R = getList(); R;
         R = R->getNext()) {
      if (R->getName() == Name) {
        Ctor = R->getCtor();
        break;
      }
    }
    assert(Ctor && "Unregistered pass name");
    setDefault(Ctor);
  }
  void setListener(MachinePassRegistryListener<PassCtorTy> *L) {
    InvocationState &S = *State;
    if (S.Listener == L)
      return;
    if (S.Listener && L)
      report_fatal_error(
          "machine-pass registry already has a listener in this invocation");
    S.Listener = L;
  }

  /// Add - Adds a function pass to the registration list.
  ///
  void Add(MachinePassRegistryNode<PassCtorTy> *Node) {
    {
      ProcessWideRegistryMutation Mutation("machine-pass registry");
      Node->setNext(List);
      List = Node;
    }
    if (InvocationState *S = getStateIfConstructed(); S && S->Listener)
      S->Listener->NotifyAdd(Node->getName(), Node->getCtor(),
                             Node->getDescription());
  }

  /// Remove - Removes a function pass from the registration list.
  ///
  void Remove(MachinePassRegistryNode<PassCtorTy> *Node) {
    bool Removed = false;
    {
      ProcessWideRegistryMutation Mutation("machine-pass registry");
      for (MachinePassRegistryNode<PassCtorTy> **I = &List; *I;
           I = (*I)->getNextAddress()) {
        if (*I == Node) {
          *I = (*I)->getNext();
          Removed = true;
          break;
        }
      }
    }
    if (Removed)
      if (InvocationState *S = getStateIfConstructed(); S && S->Listener)
        S->Listener->NotifyRemove(Node->getName());
  }
};

//===----------------------------------------------------------------------===//
///
/// RegisterPassParser class - Handle the addition of new machine passes.
///
//===----------------------------------------------------------------------===//
template <class RegistryClass>
class RegisterPassParser
    : public MachinePassRegistryListener<
          typename RegistryClass::FunctionPassCtor>,
      public cl::parser<typename RegistryClass::FunctionPassCtor> {
public:
  RegisterPassParser(cl::Option &O)
      : cl::parser<typename RegistryClass::FunctionPassCtor>(O) {}
  ~RegisterPassParser() override { RegistryClass::setListener(nullptr); }

  void initialize() {
    cl::parser<typename RegistryClass::FunctionPassCtor>::initialize();

    // Add existing passes to option.
    for (RegistryClass *Node = RegistryClass::getList();
         Node; Node = Node->getNext()) {
      this->addLiteralOption(Node->getName(),
                      (typename RegistryClass::FunctionPassCtor)Node->getCtor(),
                             Node->getDescription());
    }

    // Make sure we listen for list changes.
    RegistryClass::setListener(this);
  }

  // Implement the MachinePassRegistryListener callbacks.
  void NotifyAdd(StringRef N, typename RegistryClass::FunctionPassCtor C,
                 StringRef D) override {
    this->addLiteralOption(N, C, D);
  }
  void NotifyRemove(StringRef N) override {
    this->removeLiteralOption(N);
  }
};

} // end namespace llvm

#endif // LLVM_CODEGEN_MACHINEPASSREGISTRY_H
