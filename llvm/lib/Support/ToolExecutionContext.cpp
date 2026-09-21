//===- ToolExecutionContext.cpp - Propagate invocation state --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/ToolExecutionContext.h"

#include <cassert>
#include <utility>

using namespace llvm;

ToolExecutionContext ToolExecutionContext::capture() {
  ToolExecutionContext Context;
  if (!tryCapture(Context))
    StaticArenaToken::failCapture();
  return Context;
}

bool ToolExecutionContext::tryCapture(ToolExecutionContext &Result) {
  assert(Result.empty() && "tool execution capture result must be empty");

  ToolExecutionContext Candidate;
  // Combined capture and close use this order everywhere: command line, then
  // arena. The locks are not nested. Return the first lease before reporting a
  // failed second acquire so public capture remains fail-closed without a
  // leaked command-line lease.
  Candidate.CommandLine = cl::ContextToken::capture();
  if (!StaticArenaToken::tryCapture(Candidate.Arena)) {
    Candidate.CommandLine = cl::ContextToken();
    return false;
  }

  Result = std::move(Candidate);
  return true;
}

ScopedToolExecutionContext::ScopedToolExecutionContext(
    ToolExecutionContext Context)
    : ArenaBinding(std::move(Context.Arena)),
      CommandLineBinding(std::move(Context.CommandLine)) {}
