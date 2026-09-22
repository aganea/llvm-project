//===- ProcessWideRegistry.h - Shared registry policy ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_PROCESSWIDEREGISTRY_H
#define LLVM_SUPPORT_PROCESSWIDEREGISTRY_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"

namespace llvm {

/// Permits the current thread to perform one synchronized, lazy population of
/// process-wide registries while unrelated tool invocations are alive.
///
/// This is an expert-only bootstrap mechanism. The caller must ensure that
/// concurrent invocations cannot consume the catalogs being populated and
/// must independently serialize the complete initialization sequence. A
/// registry freeze is never bypassed. Ordinary late registration must not use
/// this class and remains fail-closed while invocations overlap or tear down.
class LLVM_ABI ProcessWideRegistryBootstrap {
public:
  ProcessWideRegistryBootstrap();
  ~ProcessWideRegistryBootstrap();

  ProcessWideRegistryBootstrap(const ProcessWideRegistryBootstrap &) = delete;
  ProcessWideRegistryBootstrap &
  operator=(const ProcessWideRegistryBootstrap &) = delete;

private:
  bool Acquired = false;
};

/// Holds the process-wide catalog mutation lock for one complete write.
///
/// Registries may be populated during process startup or serialized setup.
/// Once tool invocations overlap, adding a new entry would change the view of
/// every invocation and may race readers.  Keep this object alive until the
/// registry write is complete: checking the policy and releasing its lock
/// before the write would leave a race with another invocation starting.
class LLVM_ABI ProcessWideRegistryMutation {
public:
  explicit ProcessWideRegistryMutation(StringRef RegistryName);
  ~ProcessWideRegistryMutation();

  ProcessWideRegistryMutation(const ProcessWideRegistryMutation &) = delete;
  ProcessWideRegistryMutation &
  operator=(const ProcessWideRegistryMutation &) = delete;

private:
  bool Acquired = false;
};

/// Freezes immutable process-wide catalogs for the lifetime of this object.
///
/// Hosts that intentionally disallow late plugin or target registration may
/// construct this after completing process-wide setup. Unlike an
/// ExclusiveProcessServiceLease, a registry freeze does not serialize tool
/// invocations; it only rejects later catalog mutations, including mutations
/// attempted by threads that are not attached to an invocation context.
class LLVM_ABI ProcessWideRegistryFreeze {
public:
  ProcessWideRegistryFreeze();
  ~ProcessWideRegistryFreeze();

  ProcessWideRegistryFreeze(const ProcessWideRegistryFreeze &) = delete;
  ProcessWideRegistryFreeze &
  operator=(const ProcessWideRegistryFreeze &) = delete;

private:
  bool Acquired = false;
};

/// Grants a process service exclusive use of the current tool invocation.
///
/// Some process-wide facilities cannot identify or safely serve more than one
/// tool invocation at a time. Acquiring this lease is permitted for standalone
/// use (with no explicit command-line context) or while exactly one explicit
/// context is alive. It fails closed if invocations already overlap. While any
/// lease is held, another explicit context cannot start and the existing
/// context cannot be torn down.
class LLVM_ABI ExclusiveProcessServiceLease {
public:
  explicit ExclusiveProcessServiceLease(StringRef ServiceName);
  ~ExclusiveProcessServiceLease();

  ExclusiveProcessServiceLease(const ExclusiveProcessServiceLease &) = delete;
  ExclusiveProcessServiceLease &
  operator=(const ExclusiveProcessServiceLease &) = delete;

private:
  bool Acquired = false;
};

} // namespace llvm

#endif // LLVM_SUPPORT_PROCESSWIDEREGISTRY_H
