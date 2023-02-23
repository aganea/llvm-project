//===- CommonLinkerContext.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"

#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/Support/ThreadPool.h"

using namespace llvm;
using namespace lld;

// Reference to the current LLD instance. This is thread-local because LLD
// supports linking with multiple contexts in a global thread pool.
thread_local CommonLinkerContext *lctx;

CommonLinkerContext::CommonLinkerContext() {
  lctx = this;
  // Fire off the static initializations in CGF's constructor.
  codegen::RegisterCodeGenFlags CGF;
  // Instruct the global ThreadPool to call us whenever a task is spawed on the
  // same thread after this context was created. This allows setting a
  // thread-local pointer to this context. This is used in some rare cases where
  // we can't carry ctx around on the callstack, for example when calling
  // lld::error().
  tctx.PreTask = [ctx = this]() { lctx = ctx; };
  tctx.PostTask = [ctx = lctx] { lctx = ctx; };
}

CommonLinkerContext::~CommonLinkerContext() {
  assert(lctx);
  // Explicitly call the destructors since we created the objects with placement
  // new in SpecificAlloc::create().
  for (auto &it : instances)
    it.second->~SpecificAllocBase();
  lctx = nullptr;
}

CommonLinkerContext &lld::commonContext() {
  assert(lctx);
  return *lctx;
}

bool lld::hasContext() { return lctx != nullptr; }

void CommonLinkerContext::destroy() {
  if (lctx == nullptr)
    return;
  delete lctx;
}