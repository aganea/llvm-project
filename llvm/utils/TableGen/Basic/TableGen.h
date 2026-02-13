//===- TableGen.h ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared entry point for llvm-tblgen and llvm-min-tblgen.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/ArrayRef.h"

namespace llvm {
class ToolContext;
}
int tblgen_main(llvm::ArrayRef<const char *> Args, const llvm::ToolContext &);
