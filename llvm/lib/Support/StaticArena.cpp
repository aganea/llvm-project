//===- StaticArena.cpp - Per-invocation static storage --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/StaticArena.h"

#include "llvm/Support/MemAlloc.h"
#include "llvm/Support/Memory.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#if !LLVM_ADDRESS_SANITIZER_BUILD && (defined(_WIN32) || defined(__ELF__))
#define LLVM_STATIC_ARENA_HAS_MAPPED_STORAGE 1
#else
#define LLVM_STATIC_ARENA_HAS_MAPPED_STORAGE 0
#endif

using namespace llvm;

#if defined(__ELF__) || defined(__wasm__)
// Keep the linker-synthesized range symbols in the global namespace. GCC
// gives C-linkage declarations inside an unnamed namespace internal linkage,
// which leaves references to mangled, undefined symbols and makes the runtime
// see an empty record range.
extern "C" LLVMStaticArenaVarV1 *const __start_llvma_v1[] __attribute__((weak));
extern "C" LLVMStaticArenaVarV1 *const __stop_llvma_v1[] __attribute__((weak));
#endif

namespace {

enum class LayoutState : uint8_t { UnlaidOut, Ready };

static std::once_flag LayoutOnce;
static std::atomic<LayoutState> LayoutStatus{LayoutState::UnlaidOut};
static size_t LayoutSize = 0;
static size_t LayoutAlign = alignof(std::max_align_t);
static LLVM_THREAD_LOCAL StaticArena *CurrentArena = nullptr;

[[noreturn]] void fail(const LLVMStaticArenaVarV1 *Record, const char *Why) {
  if (Record)
    std::fprintf(stderr, "static arena: %s '%s'\n", Why,
                 Record->Name ? Record->Name : "<unnamed>");
  else
    std::fprintf(stderr, "static arena: %s\n", Why);
  std::abort();
}

#if defined(_MSC_VER)

#pragma section(".llvma$v1$a", read)
#pragma section(".llvma$v1$z", read)
__declspec(allocate(
    ".llvma$v1$a")) static LLVMStaticArenaVarV1 *const RecordRangeStart =
    nullptr;
__declspec(allocate(
    ".llvma$v1$z")) static LLVMStaticArenaVarV1 *const RecordRangeEnd = nullptr;

static uintptr_t recordBegin() {
  return reinterpret_cast<uintptr_t>(&RecordRangeStart) +
         sizeof(LLVMStaticArenaVarV1 *);
}
static uintptr_t recordEnd() {
  return reinterpret_cast<uintptr_t>(&RecordRangeEnd);
}

#elif defined(_WIN32)

__attribute__((section(".llvma$v1$a"),
               used)) static LLVMStaticArenaVarV1 *const RecordRangeStart =
    nullptr;
__attribute__((section(".llvma$v1$z"),
               used)) static LLVMStaticArenaVarV1 *const RecordRangeEnd =
    nullptr;

static uintptr_t recordBegin() {
  return reinterpret_cast<uintptr_t>(&RecordRangeStart) +
         sizeof(LLVMStaticArenaVarV1 *);
}
static uintptr_t recordEnd() {
  return reinterpret_cast<uintptr_t>(&RecordRangeEnd);
}

#elif defined(__ELF__) || defined(__wasm__)

static uintptr_t recordBegin() {
  return reinterpret_cast<uintptr_t>(__start_llvma_v1);
}
static uintptr_t recordEnd() {
  return reinterpret_cast<uintptr_t>(__stop_llvma_v1);
}

#endif

struct RecordRange {
  uintptr_t BeginAddress = 0;
  size_t Count = 0;
};

// COFF constructs the range by placing contributions between two scalar
// sentinels.  Retaining a pointer one past RecordRangeStart and indexing it as
// an array is undefined C++, even though the linker lays out valid pointer
// slots there.  In optimized builds that lets the compiler remove the range
// walk entirely.  Keep the linker-provided address as an integer and copy the
// slot behind a noinline boundary so its provenance cannot be folded back to
// the scalar sentinel (including under LTO).
static LLVM_ATTRIBUTE_NOINLINE LLVMStaticArenaVarV1 *
loadRecord(uintptr_t Address) {
  LLVMStaticArenaVarV1 *Record = nullptr;
  std::memcpy(&Record, reinterpret_cast<const void *>(Address), sizeof(Record));
  return Record;
}

static LLVMStaticArenaVarV1 *recordAt(const RecordRange &Range, size_t Index) {
  assert(Index < Range.Count && "static-arena record index out of range");
  return loadRecord(Range.BeginAddress +
                    Index * sizeof(LLVMStaticArenaVarV1 *));
}

static RecordRange getRecordRange() {
#if !defined(_WIN32) && !defined(__ELF__) && !defined(__wasm__)
  fail(nullptr,
       "the v1 runtime supports arena-producing COFF, ELF and Wasm images "
       "only");
#else
  uintptr_t BeginAddress = recordBegin();
  uintptr_t EndAddress = recordEnd();
  if ((BeginAddress == 0) != (EndAddress == 0))
    fail(nullptr, "inconsistent record-section bounds");
  if (BeginAddress == 0)
    return {};
  if (BeginAddress % alignof(LLVMStaticArenaVarV1 *) != 0 ||
      EndAddress % alignof(LLVMStaticArenaVarV1 *) != 0)
    fail(nullptr, "misaligned record-section bounds");
  if (EndAddress < BeginAddress)
    fail(nullptr, "reversed record-section bounds");
  uintptr_t ByteSize = EndAddress - BeginAddress;
  if (ByteSize % sizeof(LLVMStaticArenaVarV1 *) != 0)
    fail(nullptr, "record-section size is not pointer-aligned");
  if (ByteSize / sizeof(LLVMStaticArenaVarV1 *) >
      std::numeric_limits<size_t>::max())
    fail(nullptr, "record-section range does not fit size_t");

  return {BeginAddress,
          static_cast<size_t>(ByteSize / sizeof(LLVMStaticArenaVarV1 *))};
#endif
}

static bool isPowerOfTwo(uint64_t Value) {
  return Value != 0 && (Value & (Value - 1)) == 0;
}

static void computeLayout() {
  RecordRange Range = getRecordRange();
  std::vector<LLVMStaticArenaVarV1 *> Records;
  Records.reserve(Range.Count);

  for (size_t I = 0; I != Range.Count; ++I)
    if (LLVMStaticArenaVarV1 *Record = recordAt(Range, I))
      Records.push_back(Record);

  // The same record can be retained through more than one section entry.
  // Detect that before assigning offsets. Sorting keeps this O(N log N)
  // instead of comparing every record with every preceding record.
  std::sort(Records.begin(), Records.end(),
            std::less<LLVMStaticArenaVarV1 *>());
  auto Duplicate = std::adjacent_find(Records.begin(), Records.end());
  if (Duplicate != Records.end())
    fail(*Duplicate, "duplicate record for");

  uint64_t Cursor = 0;
  size_t MaxAlign = alignof(std::max_align_t);
  for (size_t I = 0; I != Range.Count; ++I) {
    LLVMStaticArenaVarV1 *Record = recordAt(Range, I);
    if (!Record)
      continue;

    if (Record->Offset != UINT64_MAX)
      fail(Record, "record offset was assigned before layout for");
    if (Record->Size == 0)
      fail(Record, "zero-sized record for");
    if (!isPowerOfTwo(Record->Align))
      fail(Record, "invalid alignment for");
    if (Record->Size > std::numeric_limits<size_t>::max() ||
        Record->Align > std::numeric_limits<size_t>::max())
      fail(Record, "record does not fit size_t for");

    uint64_t AlignmentAdjustment = Record->Align - 1;
    if (Cursor > UINT64_MAX - AlignmentAdjustment)
      fail(Record, "layout alignment overflow for");
    Cursor = (Cursor + AlignmentAdjustment) & ~AlignmentAdjustment;
    if (Cursor > UINT64_MAX - Record->Size)
      fail(Record, "layout size overflow for");

    Record->Offset = Cursor;
    Cursor += Record->Size;
    MaxAlign = std::max(MaxAlign, static_cast<size_t>(Record->Align));
  }

  if (Cursor > std::numeric_limits<size_t>::max())
    fail(nullptr, "static-arena layout does not fit size_t");
  LayoutSize = static_cast<size_t>(Cursor);
  LayoutAlign = MaxAlign;
  LayoutStatus.store(LayoutState::Ready, std::memory_order_release);
}

static void ensureLayout() { std::call_once(LayoutOnce, computeLayout); }

static bool ensureLayoutAndReport() {
  bool ComputedLayout = false;
  std::call_once(LayoutOnce, [&] {
    ComputedLayout = true;
    computeLayout();
  });
  return ComputedLayout;
}

static size_t allocationSizeForLayout() {
  size_t Size = std::max<size_t>(LayoutSize, 1);
  size_t Adjustment = LayoutAlign - 1;
  if (!isPowerOfTwo(LayoutAlign) || Size > SIZE_MAX - Adjustment)
    fail(nullptr, "static-arena allocation size overflow");
  return (Size + Adjustment) & ~Adjustment;
}

template <bool CollectStats> class CreateProfiler {
public:
  LLVM_ATTRIBUTE_ALWAYS_INLINE
  explicit CreateProfiler(StaticArenaCreateStats *) {}
  LLVM_ATTRIBUTE_ALWAYS_INLINE void beginPhase() {}
  LLVM_ATTRIBUTE_ALWAYS_INLINE
  void endPhase(uint64_t StaticArenaCreateStats::*) {}
  LLVM_ATTRIBUTE_ALWAYS_INLINE void finish() {}
};

template <> class CreateProfiler<true> {
  using Clock = std::chrono::steady_clock;

public:
  explicit CreateProfiler(StaticArenaCreateStats *Stats) : Stats(Stats) {
    *Stats = {};
    TotalStart = Clock::now();
  }

  void beginPhase() { PhaseStart = Clock::now(); }

  void endPhase(uint64_t StaticArenaCreateStats::*Field) {
    Stats->*Field = elapsed(PhaseStart);
  }

  void finish() { Stats->TotalNanoseconds = elapsed(TotalStart); }

private:
  static uint64_t elapsed(Clock::time_point Start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                                Start)
        .count();
  }

  StaticArenaCreateStats *Stats;
  Clock::time_point TotalStart;
  Clock::time_point PhaseStart;
};

} // namespace

class StaticArena::Impl {
public:
  enum class State : uint8_t { Live, Closing, Finalizing, Finalized };
  using Destructor = std::pair<void (*)(void *), void *>;

  void *Storage = nullptr;
  size_t AllocationSize = 0;
  size_t BackingSize = 0;
  size_t ObjectSpanSize = 0;
  size_t Alignment = 0;
#if LLVM_STATIC_ARENA_HAS_MAPPED_STORAGE
  sys::MemoryBlock MappedStorage;
#endif
  bool UsesMappedStorage = false;

  std::mutex Mutex;
  std::vector<Destructor> Destructors;
  size_t RegisteredCallbacks = 0;
  size_t ExecutedCallbacks = 0;
  size_t Attachments = 0;
  size_t OwnerBindings = 0;
  size_t OutstandingTasks = 0;
  std::atomic<State> CurrentState{State::Live};
};

StaticArena::StaticArena(std::unique_ptr<Impl> PImpl)
    : PImpl(std::move(PImpl)) {}

template <bool CollectStats>
std::unique_ptr<StaticArena>
StaticArena::createImpl(StaticArenaCreateStats *Stats) {
  CreateProfiler<CollectStats> Profiler(Stats);
  Profiler.beginPhase();
  if constexpr (CollectStats)
    Stats->ComputedLayout = ensureLayoutAndReport();
  else
    ensureLayout();
  Profiler.endPhase(&StaticArenaCreateStats::LayoutNanoseconds);
  if (LayoutStatus.load(std::memory_order_acquire) != LayoutState::Ready)
    fail(nullptr, "layout initialization did not become ready");

  auto PImpl = std::make_unique<Impl>();
  PImpl->ObjectSpanSize = LayoutSize;
  PImpl->Alignment = LayoutAlign;
  PImpl->AllocationSize = allocationSizeForLayout();

  Profiler.beginPhase();

  // Fresh anonymous mappings are demand-zero: untouched arena pages need not
  // be faulted in merely to establish the C++ zero-initialization guarantee.
  // Keep heap allocation under ASan so an escaped raw address still diagnoses
  // as use-after-free/use-after-poison after arena release. Wasm retains the
  // portable heap path because it has no host virtual-memory demand paging.
#if LLVM_STATIC_ARENA_HAS_MAPPED_STORAGE
  constexpr size_t DemandZeroMappingThreshold = 64 * 1024;
  // Keep every request far enough below SIZE_MAX for the platform mapping
  // implementation to round it to its page or allocation granularity.
  constexpr size_t MaxMappedAllocation = SIZE_MAX / 2;
  if (PImpl->AllocationSize >= DemandZeroMappingThreshold &&
      PImpl->AllocationSize <= MaxMappedAllocation) {
    auto AllocateMapping = [&](size_t Size) {
      std::error_code EC;
      return sys::Memory::allocateMappedMemory(
          Size, nullptr, sys::Memory::MF_READ | sys::Memory::MF_WRITE, EC);
    };
    auto AlignMapping = [&] {
      void *Storage = PImpl->MappedStorage.base();
      size_t Space = PImpl->MappedStorage.allocatedSize();
      return std::align(PImpl->Alignment, PImpl->AllocationSize, Storage,
                        Space);
    };

    // Mappings already satisfy the normal page-sized alignment cases. Avoid
    // making a tiny alignment allowance round the request up by a whole page.
    PImpl->MappedStorage = AllocateMapping(PImpl->AllocationSize);
    if (PImpl->MappedStorage.base()) {
      PImpl->Storage = AlignMapping();
      if (!PImpl->Storage) {
        if (std::error_code EC =
                sys::Memory::releaseMappedMemory(PImpl->MappedStorage))
          fail(nullptr, "static-arena mapped storage release failed");

        size_t AlignmentPadding = PImpl->Alignment - 1;
        if (PImpl->AllocationSize <= MaxMappedAllocation - AlignmentPadding) {
          PImpl->MappedStorage =
              AllocateMapping(PImpl->AllocationSize + AlignmentPadding);
          if (PImpl->MappedStorage.base()) {
            PImpl->Storage = AlignMapping();
            if (!PImpl->Storage)
              fail(nullptr, "mapped static-arena storage could not be aligned");
          }
        }
      }
      if (PImpl->Storage) {
        PImpl->BackingSize = PImpl->MappedStorage.allocatedSize();
        PImpl->UsesMappedStorage = true;
      }
    }
  }
#endif

  if (!PImpl->Storage) {
    PImpl->Storage = allocate_buffer(PImpl->AllocationSize, PImpl->Alignment);
    if (!PImpl->Storage)
      fail(nullptr, "static-arena allocation failed");
    PImpl->BackingSize = PImpl->AllocationSize;
  }
  Profiler.endPhase(&StaticArenaCreateStats::AllocationNanoseconds);

  __asan_unpoison_memory_region(PImpl->Storage, PImpl->AllocationSize);

  if (!PImpl->UsesMappedStorage) {
    Profiler.beginPhase();
    std::memset(PImpl->Storage, 0, PImpl->AllocationSize);
    Profiler.endPhase(&StaticArenaCreateStats::ZeroFillNanoseconds);
  }

  Profiler.beginPhase();
  RecordRange Range = getRecordRange();
  for (size_t I = 0; I != Range.Count; ++I) {
    LLVMStaticArenaVarV1 *Record = recordAt(Range, I);
    if (!Record)
      continue;
    if constexpr (CollectStats)
      ++Stats->RecordCount;
    if (Record->Offset == UINT64_MAX ||
        Record->Offset > PImpl->ObjectSpanSize ||
        Record->Size >
            PImpl->ObjectSpanSize - static_cast<size_t>(Record->Offset))
      fail(Record, "record is unassigned or out of bounds for");
    if (!Record->Templ)
      continue;
    if constexpr (CollectStats)
      Stats->TemplateBytes += Record->Size;
    std::memcpy(static_cast<char *>(PImpl->Storage) + Record->Offset,
                Record->Templ, static_cast<size_t>(Record->Size));
  }
  Profiler.endPhase(&StaticArenaCreateStats::RecordInitializationNanoseconds);

  if (PImpl->ObjectSpanSize != PImpl->AllocationSize) {
    __asan_poison_memory_region(static_cast<char *>(PImpl->Storage) +
                                    PImpl->ObjectSpanSize,
                                PImpl->AllocationSize - PImpl->ObjectSpanSize);
  }

  if constexpr (CollectStats) {
    Stats->ObjectBytes = LayoutSize;
    Stats->AllocationBytes = PImpl->AllocationSize;
    Stats->BackingBytes = PImpl->BackingSize;
    Stats->UsedDemandZeroMapping = PImpl->UsesMappedStorage;
  }

  std::unique_ptr<StaticArena> Arena(new StaticArena(std::move(PImpl)));
  Profiler.finish();
  return Arena;
}

std::unique_ptr<StaticArena> StaticArena::create() {
  return createImpl<false>(nullptr);
}

std::unique_ptr<StaticArena>
StaticArena::create(StaticArenaCreateStats &Stats) {
  return createImpl<true>(&Stats);
}

StaticArena::~StaticArena() {
  if (!PImpl)
    return;
  {
    std::lock_guard<std::mutex> Lock(PImpl->Mutex);
    if (PImpl->CurrentState.load(std::memory_order_relaxed) !=
            Impl::State::Finalized ||
        PImpl->Attachments != 0 || PImpl->OwnerBindings != 0 ||
        PImpl->OutstandingTasks != 0)
      fail(nullptr, "arena released before finalization and quiescence");
  }
  if (CurrentArena == this)
    fail(nullptr, "arena released while still current");

  __asan_poison_memory_region(PImpl->Storage, PImpl->AllocationSize);
#if LLVM_STATIC_ARENA_HAS_MAPPED_STORAGE
  if (PImpl->UsesMappedStorage) {
    if (std::error_code EC =
            sys::Memory::releaseMappedMemory(PImpl->MappedStorage))
      fail(nullptr, "static-arena mapped storage release failed");
    return;
  }
#endif
  deallocate_buffer(PImpl->Storage, PImpl->AllocationSize, PImpl->Alignment);
}

void StaticArena::beginClosing() {
  std::lock_guard<std::mutex> Lock(PImpl->Mutex);
  if (CurrentArena != this || PImpl->Attachments != 1 ||
      PImpl->OwnerBindings != 1 || PImpl->OutstandingTasks != 0 ||
      PImpl->CurrentState.load(std::memory_order_relaxed) != Impl::State::Live)
    fail(nullptr, "arena is not owner-bound and quiescent for closing");
  PImpl->CurrentState.store(Impl::State::Closing, std::memory_order_release);
}

void StaticArena::runDestructors() {
  {
    std::lock_guard<std::mutex> Lock(PImpl->Mutex);
    if (CurrentArena != this || PImpl->Attachments != 1 ||
        PImpl->OwnerBindings != 1 || PImpl->OutstandingTasks != 0 ||
        PImpl->CurrentState.load(std::memory_order_relaxed) !=
            Impl::State::Closing)
      fail(nullptr, "arena is not quiescent for finalization");
    PImpl->CurrentState.store(Impl::State::Finalizing,
                              std::memory_order_release);
  }

  for (;;) {
    Impl::Destructor D;
    {
      std::lock_guard<std::mutex> Lock(PImpl->Mutex);
      if (PImpl->Destructors.empty()) {
        if (PImpl->ExecutedCallbacks != PImpl->RegisteredCallbacks)
          fail(nullptr, "destructor callback count mismatch");
        PImpl->CurrentState.store(Impl::State::Finalized,
                                  std::memory_order_release);
        return;
      }
      D = PImpl->Destructors.back();
      PImpl->Destructors.pop_back();
    }

    D.first(D.second);

    {
      std::lock_guard<std::mutex> Lock(PImpl->Mutex);
      ++PImpl->ExecutedCallbacks;
    }
  }
}

StaticArenaToken::StaticArenaToken(StaticArena *Arena) : Arena(Arena) {
  if (!Arena)
    return;
  std::lock_guard<std::mutex> Lock(Arena->PImpl->Mutex);
  if (Arena->PImpl->CurrentState.load(std::memory_order_relaxed) !=
      StaticArena::Impl::State::Live)
    fail(nullptr, "capturing a closing static arena");
  ++Arena->PImpl->OutstandingTasks;
}

StaticArenaToken::StaticArenaToken(const StaticArenaToken &Other)
    : StaticArenaToken(Other.Arena) {}

StaticArenaToken::StaticArenaToken(StaticArenaToken &&Other) noexcept
    : Arena(std::exchange(Other.Arena, nullptr)) {}

StaticArenaToken &StaticArenaToken::operator=(const StaticArenaToken &Other) {
  if (this == &Other)
    return *this;
  StaticArenaToken Replacement(Other);
  *this = std::move(Replacement);
  return *this;
}

StaticArenaToken &
StaticArenaToken::operator=(StaticArenaToken &&Other) noexcept {
  if (this == &Other)
    return *this;
  reset();
  Arena = std::exchange(Other.Arena, nullptr);
  return *this;
}

StaticArenaToken::~StaticArenaToken() { reset(); }

StaticArenaToken StaticArenaToken::capture() {
  StaticArenaToken Result;
  if (!tryCapture(Result))
    failCapture();
  return Result;
}

bool StaticArenaToken::tryCapture(StaticArenaToken &Result) {
  assert(!Result.Arena && "static-arena capture result must be empty");
  StaticArena *Arena = CurrentArena;
  if (!Arena)
    return true;

  std::lock_guard<std::mutex> Lock(Arena->PImpl->Mutex);
  if (Arena->PImpl->CurrentState.load(std::memory_order_relaxed) !=
      StaticArena::Impl::State::Live)
    return false;
  ++Arena->PImpl->OutstandingTasks;
  Result.Arena = Arena;
  return true;
}

void StaticArenaToken::failCapture() {
  fail(nullptr, "capturing a closing static arena");
}

void StaticArenaToken::reset() {
  if (!Arena)
    return;
  StaticArena *OldArena = std::exchange(Arena, nullptr);
  std::lock_guard<std::mutex> Lock(OldArena->PImpl->Mutex);
  if (OldArena->PImpl->OutstandingTasks == 0)
    fail(nullptr, "static-arena task lease underflow");
  --OldArena->PImpl->OutstandingTasks;
}

ScopedStaticArenaBinding::ScopedStaticArenaBinding(StaticArena &Arena) {
  bind(&Arena, true);
}

ScopedStaticArenaBinding::ScopedStaticArenaBinding(StaticArenaToken Token)
    : Lease(std::move(Token)) {
  bind(Lease.Arena, false);
}

void ScopedStaticArenaBinding::bind(StaticArena *Arena, bool Owner) {
  if (Arena) {
    std::lock_guard<std::mutex> Lock(Arena->PImpl->Mutex);
    if (Arena->PImpl->CurrentState.load(std::memory_order_relaxed) !=
        StaticArena::Impl::State::Live)
      fail(nullptr, "binding a closing static arena");
    if (Owner && Arena->PImpl->OwnerBindings != 0)
      fail(nullptr, "static arena already has an owner binding");
    ++Arena->PImpl->Attachments;
    if (Owner)
      ++Arena->PImpl->OwnerBindings;
  }
  Bound = Arena;
  IsOwner = Owner;
  Previous = CurrentArena;
  CurrentArena = Arena;
  IsInstalled = true;
}

ScopedStaticArenaBinding::~ScopedStaticArenaBinding() {
  if (!IsInstalled)
    return;
  if (CurrentArena != Bound)
    fail(nullptr, "static-arena bindings destroyed out of order");
  CurrentArena = Previous;

  if (!Bound)
    return;
  std::lock_guard<std::mutex> Lock(Bound->PImpl->Mutex);
  if (Bound->PImpl->Attachments == 0)
    fail(nullptr, "static-arena attachment underflow");
  --Bound->PImpl->Attachments;
  if (IsOwner) {
    if (Bound->PImpl->OwnerBindings == 0)
      fail(nullptr, "static-arena owner-binding underflow");
    --Bound->PImpl->OwnerBindings;
  }
}

bool llvm::isInCurrentStaticArena(const void *Pointer) {
  if (!CurrentArena || !Pointer)
    return false;
  const StaticArena::Impl &PImpl = *CurrentArena->PImpl;
  uintptr_t Begin = reinterpret_cast<uintptr_t>(PImpl.Storage);
  uintptr_t Address = reinterpret_cast<uintptr_t>(Pointer);
  return Address >= Begin && Address - Begin < PImpl.ObjectSpanSize;
}

bool llvm::hasCurrentStaticArena() { return CurrentArena != nullptr; }

const void *llvm::getCurrentStaticArenaIdentity() { return CurrentArena; }

extern "C" void *__llvm_arena_addr_v1(const LLVMStaticArenaVarV1 *Record) {
  if (LayoutStatus.load(std::memory_order_acquire) != LayoutState::Ready)
    fail(Record, "layout is not ready while accessing");
  if (!Record)
    fail(nullptr, "null static-arena record");

  StaticArena *Arena = CurrentArena;
  if (!Arena)
    fail(Record, "no invocation is attached while accessing");
  StaticArena::Impl &PImpl = *Arena->PImpl;
  StaticArena::Impl::State State =
      PImpl.CurrentState.load(std::memory_order_acquire);
  if (State != StaticArena::Impl::State::Live &&
      State != StaticArena::Impl::State::Closing &&
      State != StaticArena::Impl::State::Finalizing)
    fail(Record, "inactive arena while accessing");

  if (Record->Offset == UINT64_MAX || Record->Offset > PImpl.ObjectSpanSize ||
      Record->Size > PImpl.ObjectSpanSize - static_cast<size_t>(Record->Offset))
    fail(Record, "record is unassigned or out of bounds for");
  return static_cast<char *>(PImpl.Storage) + Record->Offset;
}

extern "C" int __llvm_arena_atexit_v1(void (*Destroy)(void *), void *Object) {
  StaticArena *Arena = CurrentArena;
  if (!Arena)
    fail(nullptr, "destructor registration with no arena");
  if (!Destroy)
    fail(nullptr, "null static-arena destructor callback");
  if (Object && !isInCurrentStaticArena(Object))
    fail(nullptr, "destructor object is outside the current static arena");

  std::lock_guard<std::mutex> Lock(Arena->PImpl->Mutex);
  StaticArena::Impl::State State =
      Arena->PImpl->CurrentState.load(std::memory_order_relaxed);
  if (State != StaticArena::Impl::State::Live &&
      State != StaticArena::Impl::State::Closing &&
      State != StaticArena::Impl::State::Finalizing)
    fail(nullptr, "destructor registration on inactive arena");
  if (State != StaticArena::Impl::State::Live &&
      (Arena->PImpl->Attachments != 1 || Arena->PImpl->OwnerBindings != 1 ||
       Arena->PImpl->OutstandingTasks != 0))
    fail(nullptr, "destructor registration by non-owner during finalization");

  Arena->PImpl->Destructors.emplace_back(Destroy, Object);
  ++Arena->PImpl->RegisteredCallbacks;
  return 0;
}
