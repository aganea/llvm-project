//===- llvm/CodeGen/RegAllocRegistry.h --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the implementation for register allocator function
// pass registry (RegisterRegAlloc).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_REGALLOCREGISTRY_H
#define LLVM_CODEGEN_REGALLOCREGISTRY_H

#include "llvm/CodeGen/MachinePassRegistry.h"
#include "llvm/CodeGen/RegAllocCommon.h"

namespace llvm {

class FunctionPass;

//===----------------------------------------------------------------------===//
///
/// RegisterRegAllocBase class - Track the registration of register allocators.
///
//===----------------------------------------------------------------------===//
template <class SubClass>
class RegisterRegAllocBase
    : public MachinePassRegistryNode<FunctionPass *(*)()> {
public:
  using FunctionPassCtor = FunctionPass *(*)();

private:
  /// Return this allocator family's process-wide registration catalog.
  ///
  /// MachinePassRegistry keeps selection and listener state in the current
  /// invocation, but the available allocators are immutable process metadata
  /// once registration is complete. Keeping the catalog behind an accessor
  /// also avoids instantiating a mutable template static data member for every
  /// allocator family.
  static MachinePassRegistry<FunctionPassCtor> &registry() {
    static MachinePassRegistry<FunctionPassCtor> Registry;
    return Registry;
  }

public:
  RegisterRegAllocBase(const char *N, const char *D, FunctionPassCtor C)
      : MachinePassRegistryNode(N, D, C) {
    registry().Add(this);
  }

  ~RegisterRegAllocBase() { registry().Remove(this); }

  // Accessors.
  SubClass *getNext() const {
    return static_cast<SubClass *>(MachinePassRegistryNode::getNext());
  }

  static SubClass *getList() {
    return static_cast<SubClass *>(registry().getList());
  }

  static FunctionPassCtor getDefault() { return registry().getDefault(); }

  static void setDefault(FunctionPassCtor C) { registry().setDefault(C); }

  static void setListener(MachinePassRegistryListener<FunctionPassCtor> *L) {
    registry().setListener(L);
  }
};

class RegisterRegAlloc : public RegisterRegAllocBase<RegisterRegAlloc> {
public:
  RegisterRegAlloc(const char *N, const char *D, FunctionPassCtor C)
      : RegisterRegAllocBase(N, D, C) {}
};

} // end namespace llvm

#endif // LLVM_CODEGEN_REGALLOCREGISTRY_H
