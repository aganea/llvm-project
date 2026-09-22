//===- ProcessWideRegistry.cpp - Shared registry policy ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/ProcessWideRegistry.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"

#include <mutex>

using namespace llvm;

namespace {
thread_local unsigned RegistryBootstrapDepth = 0;

struct InvocationPolicyState {
  std::mutex Mutex;
  unsigned ActiveContexts = 0;
  unsigned ClosingContexts = 0;
  unsigned RegistryFreezes = 0;
  unsigned ExclusiveServiceLeases = 0;
};

// Process-service policy must remain available until process exit, including
// while command-line contexts and other Support globals are being destroyed.
InvocationPolicyState &invocationPolicyState() {
  static auto *State = new InvocationPolicyState();
  return *State;
}
} // namespace

ProcessWideRegistryBootstrap::ProcessWideRegistryBootstrap() {
  ++RegistryBootstrapDepth;
  Acquired = true;
}

ProcessWideRegistryBootstrap::~ProcessWideRegistryBootstrap() {
  if (!Acquired)
    return;
  if (RegistryBootstrapDepth == 0)
    report_fatal_error("process-wide registry bootstrap underflow");
  --RegistryBootstrapDepth;
}

namespace llvm::detail {
void registerExplicitInvocationContext() {
  InvocationPolicyState &State = invocationPolicyState();
  bool ExclusiveServiceIsActive;
  {
    std::lock_guard<std::mutex> Lock(State.Mutex);
    ExclusiveServiceIsActive = State.ExclusiveServiceLeases != 0;
    if (!ExclusiveServiceIsActive)
      ++State.ActiveContexts;
  }
  if (ExclusiveServiceIsActive)
    report_fatal_error(
        "cannot start a tool invocation while an exclusive process service "
        "is active");
}

void beginExplicitInvocationContextTeardown() {
  InvocationPolicyState &State = invocationPolicyState();
  enum class Error { None, ContextUnderflow, ExclusiveServiceIsActive } Err =
      Error::None;
  {
    std::lock_guard<std::mutex> Lock(State.Mutex);
    if (State.ActiveContexts == 0)
      Err = Error::ContextUnderflow;
    else if (State.ExclusiveServiceLeases != 0)
      Err = Error::ExclusiveServiceIsActive;
    else
      ++State.ClosingContexts;
  }
  if (Err == Error::ContextUnderflow)
    report_fatal_error("tool invocation context count underflow");
  if (Err == Error::ExclusiveServiceIsActive)
    report_fatal_error(
        "cannot tear down a tool invocation while an exclusive process "
        "service is active");
}

void finishExplicitInvocationContextTeardown() {
  InvocationPolicyState &State = invocationPolicyState();
  bool Underflow;
  {
    std::lock_guard<std::mutex> Lock(State.Mutex);
    Underflow = State.ActiveContexts == 0 || State.ClosingContexts == 0;
    if (!Underflow) {
      --State.ClosingContexts;
      --State.ActiveContexts;
    }
  }
  if (Underflow)
    report_fatal_error("tool invocation context teardown underflow");
}

bool hasConcurrentExplicitInvocationContexts() {
  InvocationPolicyState &State = invocationPolicyState();
  std::lock_guard<std::mutex> Lock(State.Mutex);
  return State.ActiveContexts > 1;
}

void acquireProcessWideRegistryFreeze() {
  InvocationPolicyState &State = invocationPolicyState();
  std::lock_guard<std::mutex> Lock(State.Mutex);
  ++State.RegistryFreezes;
}

void releaseProcessWideRegistryFreeze() {
  InvocationPolicyState &State = invocationPolicyState();
  bool Underflow;
  {
    std::lock_guard<std::mutex> Lock(State.Mutex);
    Underflow = State.RegistryFreezes == 0;
    if (!Underflow)
      --State.RegistryFreezes;
  }
  if (Underflow)
    report_fatal_error("process-wide registry freeze underflow");
}

void acquireExclusiveProcessService(StringRef ServiceName) {
  InvocationPolicyState &State = invocationPolicyState();
  enum class Error { None, ContextIsClosing, MultipleInvocations } Err =
      Error::None;
  {
    std::lock_guard<std::mutex> Lock(State.Mutex);
    if (State.ClosingContexts != 0)
      Err = Error::ContextIsClosing;
    else if (State.ActiveContexts > 1)
      Err = Error::MultipleInvocations;
    else
      ++State.ExclusiveServiceLeases;
  }
  if (Err == Error::ContextIsClosing)
    report_fatal_error("exclusive process service '" + ServiceName +
                       "' cannot be acquired while a tool invocation is "
                       "being torn down");
  if (Err == Error::MultipleInvocations)
    report_fatal_error("exclusive process service '" + ServiceName +
                       "' cannot be acquired while multiple tool invocations "
                       "are active");
}

void releaseExclusiveProcessService() {
  InvocationPolicyState &State = invocationPolicyState();
  bool Underflow;
  {
    std::lock_guard<std::mutex> Lock(State.Mutex);
    Underflow = State.ExclusiveServiceLeases == 0;
    if (!Underflow)
      --State.ExclusiveServiceLeases;
  }
  if (Underflow)
    report_fatal_error("exclusive process service lease underflow");
}
} // namespace llvm::detail

ProcessWideRegistryMutation::ProcessWideRegistryMutation(
    StringRef RegistryName) {
  InvocationPolicyState &State = invocationPolicyState();
  std::unique_lock<std::mutex> Lock(State.Mutex);
  enum class Error {
    None,
    RegistryIsFrozen,
    ContextIsClosing,
    MultipleInvocations
  } Err = Error::None;
  if (State.RegistryFreezes != 0)
    Err = Error::RegistryIsFrozen;
  else if (State.ClosingContexts != 0 && RegistryBootstrapDepth == 0)
    Err = Error::ContextIsClosing;
  else if (State.ActiveContexts > 1 && RegistryBootstrapDepth == 0)
    Err = Error::MultipleInvocations;

  if (Err != Error::None)
    Lock.unlock();
  if (Err == Error::RegistryIsFrozen)
    report_fatal_error(RegistryName +
                       " mutated after process-wide registration was frozen; "
                       "complete registration before tool dispatch");
  if (Err == Error::ContextIsClosing)
    report_fatal_error(RegistryName +
                       " mutated while a tool invocation is being torn down; "
                       "complete process-wide registration before teardown");
  if (Err == Error::MultipleInvocations)
    report_fatal_error(
        RegistryName +
        " mutated while multiple tool invocations are active; complete "
        "process-wide registration before concurrent tool execution");

  // Transfer ownership of the locked policy mutex to this guard. Context
  // creation, teardown and registry freezing all use the same mutex, so none
  // of them can pass its policy check until the registry write is complete.
  Acquired = true;
  (void)Lock.release();
}

ProcessWideRegistryMutation::~ProcessWideRegistryMutation() {
  if (Acquired)
    invocationPolicyState().Mutex.unlock();
}

ProcessWideRegistryFreeze::ProcessWideRegistryFreeze() {
  detail::acquireProcessWideRegistryFreeze();
  Acquired = true;
}

ProcessWideRegistryFreeze::~ProcessWideRegistryFreeze() {
  if (Acquired)
    detail::releaseProcessWideRegistryFreeze();
}

ExclusiveProcessServiceLease::ExclusiveProcessServiceLease(
    StringRef ServiceName) {
  detail::acquireExclusiveProcessService(ServiceName);
  Acquired = true;
}

ExclusiveProcessServiceLease::~ExclusiveProcessServiceLease() {
  if (Acquired)
    detail::releaseExclusiveProcessService();
}
