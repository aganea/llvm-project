//===- ToolExecutionContext.h - Propagate invocation state ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_TOOLEXECUTIONCONTEXT_H
#define LLVM_SUPPORT_TOOLEXECUTIONCONTEXT_H

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/StaticArena.h"

namespace llvm {

/// The invocation state that follows a unit of work between threads.
///
/// Capture owns independent command-line and arena task leases immediately, so
/// queued work keeps both control blocks live. Copies acquire their own leases.
/// A standalone process with no explicit invocation produces an empty context.
///
/// A coroutine or fiber scheduler can store a captured context with a work
/// item and install ScopedToolExecutionContext around each resume or switch.
/// The scoped binding must live in the dispatcher frame so suspension returns
/// through its destructor. A stackful owner that yields instead needs an empty
/// scoped binding across the yield to mask its context from the dispatcher.
class LLVM_ABI ToolExecutionContext {
public:
  ToolExecutionContext() = default;
  ToolExecutionContext(const ToolExecutionContext &) = default;
  ToolExecutionContext(ToolExecutionContext &&) noexcept = default;
  ToolExecutionContext &operator=(const ToolExecutionContext &) = default;
  ToolExecutionContext &operator=(ToolExecutionContext &&) noexcept = default;

  static ToolExecutionContext capture();

  bool empty() const { return !CommandLine && !Arena; }

private:
  /// Attempts the two-part capture without failing the process. On failure,
  /// Result remains empty and the command-line lease has already been returned.
  static bool tryCapture(ToolExecutionContext &Result);

  cl::ContextToken CommandLine;
  StaticArenaToken Arena;

  friend class ScopedToolExecutionContext;
  friend class ToolExecutionContextTestPeer;
};

/// Installs one complete ToolExecutionContext for a task and restores the
/// caller's state afterwards. Arena state is installed first and restored last,
/// matching invocation construction and teardown order.
class LLVM_ABI ScopedToolExecutionContext {
public:
  explicit ScopedToolExecutionContext(ToolExecutionContext Context);
  ~ScopedToolExecutionContext() = default;

  ScopedToolExecutionContext(const ScopedToolExecutionContext &) = delete;
  ScopedToolExecutionContext &
  operator=(const ScopedToolExecutionContext &) = delete;

private:
  ScopedStaticArenaBinding ArenaBinding;
  cl::ScopedContextTokenBinding CommandLineBinding;
};

} // namespace llvm

#endif // LLVM_SUPPORT_TOOLEXECUTIONCONTEXT_H
