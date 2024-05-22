//===--- LLVMDriver.cpp - Implement the APIs for ToolContext --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/LLVMDriver.h"

using namespace llvm;

LLVM_THREAD_LOCAL ToolContext *ToolContext::Current;
