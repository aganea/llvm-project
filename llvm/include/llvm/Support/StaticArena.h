//===- StaticArena.h - Per-invocation static storage ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
///
/// Runtime support for globals emitted with Clang's experimental static-arena
/// storage mode. The compiler emits one LLVMStaticArenaVarV1 record per global
/// and calls the two versioned C entry points below. Embedders own an arena,
/// bind it while an invocation is running, and finalize it before releasing
/// the storage.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_STATICARENA_H
#define LLVM_SUPPORT_STATICARENA_H

#include "llvm/Support/Compiler.h"

#include <cstddef>
#include <cstdint>
#include <memory>

/// The compiler/runtime ABI for one arena-resident global.
///
/// Offset is initialized to UINT64_MAX by the compiler. The runtime assigns it
/// exactly once, while laying out the main executable's record section.
struct LLVMStaticArenaVarV1 {
  uint64_t Offset;
  uint64_t Size;
  uint64_t Align;
  const void *Templ;
  const char *Name;
};

static_assert(offsetof(LLVMStaticArenaVarV1, Offset) == 0);
static_assert(offsetof(LLVMStaticArenaVarV1, Size) == 8);
static_assert(offsetof(LLVMStaticArenaVarV1, Align) == 16);
static_assert(offsetof(LLVMStaticArenaVarV1, Templ) == 24);
static_assert(offsetof(LLVMStaticArenaVarV1, Name) == 24 + sizeof(void *));
static_assert(sizeof(LLVMStaticArenaVarV1) == 24 + 2 * sizeof(void *));

extern "C" LLVM_ABI void *
__llvm_arena_addr_v1(const LLVMStaticArenaVarV1 *Record);

extern "C" LLVM_ABI int __llvm_arena_atexit_v1(void (*Destroy)(void *),
                                               void *Object);

namespace llvm {

class ScopedStaticArenaBinding;
class StaticArenaToken;
class ToolExecutionContext;

/// Timings and sizes collected while creating one static arena.
///
/// This is an opt-in diagnostic interface intended for benchmarks. The normal
/// create() path does not read a clock or collect these values. Layout time is
/// normally nonzero only for the first arena created by an image; subsequent
/// calls measure the cheap call_once fast path instead.
struct StaticArenaCreateStats {
  uint64_t TotalNanoseconds = 0;
  uint64_t LayoutNanoseconds = 0;
  uint64_t AllocationNanoseconds = 0;
  uint64_t ZeroFillNanoseconds = 0;
  uint64_t RecordInitializationNanoseconds = 0;
  uint64_t RecordCount = 0;
  uint64_t TemplateBytes = 0;
  uint64_t ObjectBytes = 0;
  uint64_t AllocationBytes = 0;
  uint64_t BackingBytes = 0;
  bool ComputedLayout = false;
  bool UsedDemandZeroMapping = false;
};

/// Owns one instance of the process-wide static-arena layout.
///
/// The lifecycle is deliberately explicit. beginClosing() first proves that
/// all task leases and all bindings except the owner's have returned.
/// runDestructors() then drains callbacks in reverse registration order while
/// the owner remains bound. The destructor finally poisons and releases the
/// storage, and therefore requires the arena to have reached Finalized and to
/// have no bindings or task leases.
class LLVM_ABI StaticArena {
public:
  static std::unique_ptr<StaticArena> create();

  /// Creates an arena and records phase-level creation costs in \p Stats.
  /// Prefer create() outside diagnostics and benchmarks: it has no timing
  /// instrumentation on its implementation path.
  static std::unique_ptr<StaticArena> create(StaticArenaCreateStats &Stats);

  ~StaticArena();

  StaticArena(const StaticArena &) = delete;
  StaticArena &operator=(const StaticArena &) = delete;

  void beginClosing();
  void runDestructors();

private:
  template <bool CollectStats>
  static std::unique_ptr<StaticArena> createImpl(StaticArenaCreateStats *Stats);

  class Impl;
  explicit StaticArena(std::unique_ptr<Impl> PImpl);

  std::unique_ptr<Impl> PImpl;

  friend class ScopedStaticArenaBinding;
  friend class StaticArenaToken;
  friend void * ::__llvm_arena_addr_v1(const LLVMStaticArenaVarV1 *);
  friend int ::__llvm_arena_atexit_v1(void (*)(void *), void *);
  friend bool isInCurrentStaticArena(const void *);
};

/// A task lease for the arena currently bound to this thread.
///
/// Capturing or copying a token is permitted only while the arena is Live.
/// Each token owns one lease immediately, including while work is queued but
/// not yet running. An empty token represents a non-arena invocation.
class LLVM_ABI StaticArenaToken {
public:
  StaticArenaToken() = default;
  StaticArenaToken(const StaticArenaToken &Other);
  StaticArenaToken(StaticArenaToken &&Other) noexcept;
  StaticArenaToken &operator=(const StaticArenaToken &Other);
  StaticArenaToken &operator=(StaticArenaToken &&Other) noexcept;
  ~StaticArenaToken();

  static StaticArenaToken capture();

  explicit operator bool() const { return Arena != nullptr; }

private:
  static bool tryCapture(StaticArenaToken &Result);
  [[noreturn]] static void failCapture();

  explicit StaticArenaToken(StaticArena *Arena);
  void reset();

  StaticArena *Arena = nullptr;

  friend class ScopedStaticArenaBinding;
  friend class ToolExecutionContext;
};

/// Installs an arena in the current thread, restoring the previous binding on
/// destruction. Bindings form a strict stack.
class LLVM_ABI ScopedStaticArenaBinding {
public:
  /// Establishes an owner binding. The caller must keep \p Arena alive.
  explicit ScopedStaticArenaBinding(StaticArena &Arena);

  /// Establishes a task binding and keeps the token's lease for its lifetime.
  /// An empty token explicitly installs no arena, hiding and later restoring
  /// any arena belonging to an enclosing invocation on this thread.
  explicit ScopedStaticArenaBinding(StaticArenaToken Token);

  ~ScopedStaticArenaBinding();

  ScopedStaticArenaBinding(const ScopedStaticArenaBinding &) = delete;
  ScopedStaticArenaBinding &
  operator=(const ScopedStaticArenaBinding &) = delete;

private:
  void bind(StaticArena *Arena, bool IsOwner);

  StaticArenaToken Lease;
  StaticArena *Bound = nullptr;
  StaticArena *Previous = nullptr;
  bool IsOwner = false;
  bool IsInstalled = false;
};

/// Returns whether \p Pointer lies in the object span of the arena currently
/// bound to this thread. This query never traps; null and no binding are false.
LLVM_ABI bool isInCurrentStaticArena(const void *Pointer);

/// Returns whether this thread currently has an invocation arena bound.
///
/// Process services use this to retain a process-wide fallback for tokenless
/// threads while routing invocation-owned state through the arena whenever a
/// propagated tool context is installed.
LLVM_ABI bool hasCurrentStaticArena();

/// Returns an opaque identity for the arena bound to this thread, or null when
/// there is no binding. The identity remains valid while a captured task lease
/// or owner binding keeps that arena alive.
LLVM_ABI const void *getCurrentStaticArenaIdentity();

} // namespace llvm

#endif // LLVM_SUPPORT_STATICARENA_H
