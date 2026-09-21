// Exercise Clang-produced arena records and lifecycle entries in a
// native executable.  The fixture runtime is deliberately small and has no
// LLVM library dependency, so this remains normal check-clang coverage.
//
// REQUIRES: native, thread_support
// UNSUPPORTED: system-darwin, system-aix, system-zos
// RUN: rm -rf %t && split-file %s %t
// RUN: %clangxx -std=c++20 -O2 -ffunction-sections -fdata-sections \
// RUN:   -c %t/runtime.cpp -o %t/runtime.o
// RUN: %clangxx -std=c++20 -O2 -ffunction-sections -fdata-sections \
// RUN:   -Xclang -fstatic-arena=llvmi_e2e_common \
// RUN:   -Xclang -fstatic-arena-list=%t/arena.list \
// RUN:   -c %t/common.cpp -o %t/common.o
// RUN: %clangxx -std=c++20 -O2 -ffunction-sections -fdata-sections \
// RUN:   -Xclang -fstatic-arena=llvmi_e2e_a \
// RUN:   -Xclang -fstatic-arena-list=%t/arena.list \
// RUN:   -c %t/tool-a.cpp -o %t/tool-a.o
// RUN: %clangxx -std=c++20 -O2 -ffunction-sections -fdata-sections \
// RUN:   -Xclang -fstatic-arena=llvmi_e2e_b \
// RUN:   -Xclang -fstatic-arena-list=%t/arena.list \
// RUN:   -c %t/tool-b.cpp -o %t/tool-b.o
// RUN: %clangxx -std=c++20 -O2 -ffunction-sections -fdata-sections \
// RUN:   -c %t/main.cpp -o %t/main.o
// RUN: %clangxx -std=c++20 -O2 %if !system-windows %{ -pthread %} \
// RUN:   %t/runtime.o %t/common.o \
// RUN:   %t/tool-a.o %t/tool-b.o %t/main.o \
// RUN:   %if system-windows %{ -Wl,/OPT:REF %} \
// RUN:   %else %{ -Wl,--gc-sections %} -o %t/e2e.exe
// RUN: %t/e2e.exe | FileCheck %s
// RUN: not --crash %t/e2e.exe --unbound 2>&1 \
// RUN:   | FileCheck --check-prefix=TRAP %s
//
// CHECK: constructed-before-main=0
// CHECK: same-name-options=ok
// CHECK: concurrent-independent=ok
// CHECK: same-global-address=distinct
// CHECK: cached-pointer=ok
// CHECK: comdat-dedup=ok
// CHECK: persistent-thread-epoch=ok
// CHECK: recreation-and-dtors=ok
// CHECK: static-arena-e2e=passed
// TRAP: static arena: no invocation is attached while accessing

//--- arena.list
[arena]
src:*common.cpp
src:*tool-a.cpp
src:*tool-b.cpp

//--- fixture.h
#ifndef STATIC_ARENA_E2E_FIXTURE_H
#define STATIC_ARENA_E2E_FIXTURE_H

#include <cstddef>
#include <map>
#include <string>
#include <utility>

namespace fixture {

struct Arena;

Arena *createArena();
void destroyArena(Arena *A);
void finalizeArena(Arena *A);
std::size_t registeredDestructors(Arena *A);
std::size_t executedDestructors(Arena *A);

class Binding {
  Arena *Bound;
  Arena *Previous;

public:
  explicit Binding(Arena *A);
  ~Binding();

  Binding(const Binding &) = delete;
  Binding &operator=(const Binding &) = delete;
};

void runCommonInitializers();
void runAInitializers();
void runBInitializers();

void noteConstruction();
void noteDestruction();
int constructionCount();
int destructionCount();

class Registry {
  std::map<std::string, const void *> Options;

public:
  int Collisions = 0;

  Registry();
  ~Registry();
  void add(const std::string &Name, const void *Option);
  void remove(const std::string &Name, const void *Option);
};

Registry &registry();

template <class T> class Opt {
  T Internal{};
  T *Location;
  std::string Name;

public:
  explicit Opt(const char *N) : Location(&Internal), Name(N) {
    noteConstruction();
    registry().add(Name, this);
  }

  Opt(const char *N, T *External) : Location(External), Name(N) {
    noteConstruction();
    registry().add(Name, this);
  }

  ~Opt() {
    registry().remove(Name, this);
    noteDestruction();
  }

  Opt &operator=(T Value) {
    *Location = std::move(Value);
    return *this;
  }

  operator const T &() const { return *Location; }
};

class Label {
  std::string Value;

public:
  explicit Label(const char *Initial) : Value(Initial) { noteConstruction(); }
  ~Label() { noteDestruction(); }
  void set(const char *NewValue) { Value = NewValue; }
  const std::string &get() const { return Value; }
};

class Counted {
public:
  Counted() { noteConstruction(); }
  ~Counted() { noteDestruction(); }
};

void setCPU(const char *CPU);
std::string getCPU();
int commonTemplateValue();
void setCommonTemplateValue(int Value);
void *commonAddress();
bool commonIsOverAligned();
int registryCollisions();

void runToolA(const char *CPU);
void runToolB(const char *CPU);
void *toolAInlineAddress();
void *toolBInlineAddress();
int toolARuns();
int toolBRuns();
int toolAVerbose();
int toolBVerbose();
std::string toolALabel();
std::string toolBLabel();

} // namespace fixture

#endif

//--- runtime.cpp
#include "fixture.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

struct LLVMStaticArenaVarV1 {
  std::uint64_t Offset;
  std::uint64_t Size;
  std::uint64_t Align;
  const void *Templ;
  const char *Name;
};

namespace {

using InitFn = void (*)();

#if defined(_MSC_VER)

#pragma section(".llvma$v1$a", read)
#pragma section(".llvma$v1$z", read)
__declspec(allocate(".llvma$v1$a")) static LLVMStaticArenaVarV1 *const
    ArenaRecordsBegin = nullptr;
__declspec(allocate(".llvma$v1$z")) static LLVMStaticArenaVarV1 *const
    ArenaRecordsEnd = nullptr;

#define E2E_INIT_RANGE(Name, Section)                                        \
  __pragma(section(".llvmi$" Section "$a", read))                          \
      __pragma(section(".llvmi$" Section "$z", read))                      \
          __declspec(allocate(".llvmi$" Section "$a")) static InitFn const \
              Name##Start = nullptr;                                         \
  __declspec(allocate(".llvmi$" Section "$z")) static InitFn const         \
      Name##Stop = nullptr;                                                   \
  static InitFn const *const Name##Begin = &Name##Start + 1;                  \
  static InitFn const *const Name##End = &Name##Stop

static LLVMStaticArenaVarV1 *const *recordBegin() {
  return &ArenaRecordsBegin + 1;
}
static LLVMStaticArenaVarV1 *const *recordEnd() {
  return &ArenaRecordsEnd;
}

#elif defined(_WIN32)

__attribute__((section(".llvma$v1$a"),
               used)) static LLVMStaticArenaVarV1 *const ArenaRecordsBegin =
    nullptr;
__attribute__((section(".llvma$v1$z"),
               used)) static LLVMStaticArenaVarV1 *const ArenaRecordsEnd =
    nullptr;

#define E2E_INIT_RANGE(Name, Section)                                      \
  __attribute__((section(".llvmi$" Section "$a"), used)) static InitFn    \
      const Name##Start = nullptr;                                         \
  __attribute__((section(".llvmi$" Section "$z"), used)) static InitFn    \
      const Name##Stop = nullptr;                                          \
  static InitFn const *const Name##Begin = &Name##Start + 1;                \
  static InitFn const *const Name##End = &Name##Stop

static LLVMStaticArenaVarV1 *const *recordBegin() {
  return &ArenaRecordsBegin + 1;
}
static LLVMStaticArenaVarV1 *const *recordEnd() {
  return &ArenaRecordsEnd;
}

#elif defined(__ELF__)

extern "C" LLVMStaticArenaVarV1 *const __start_llvma_v1[]
    __attribute__((weak));
extern "C" LLVMStaticArenaVarV1 *const __stop_llvma_v1[]
    __attribute__((weak));

#define E2E_INIT_RANGE(Name, Section)                                      \
  extern "C" InitFn __start_##Name[] __attribute__((weak));               \
  extern "C" InitFn __stop_##Name[] __attribute__((weak));                \
  static InitFn const *const Name##Begin = __start_##Name;                  \
  static InitFn const *const Name##End = __stop_##Name

static LLVMStaticArenaVarV1 *const *recordBegin() {
  return __start_llvma_v1;
}
static LLVMStaticArenaVarV1 *const *recordEnd() {
  return __stop_llvma_v1;
}

#else
#error "static-arena e2e supports native COFF and ELF hosts only"
#endif

E2E_INIT_RANGE(llvmi_e2e_common, "e2e_common");
E2E_INIT_RANGE(llvmi_e2e_a, "e2e_a");
E2E_INIT_RANGE(llvmi_e2e_b, "e2e_b");

std::once_flag LayoutOnce;
std::atomic<bool> LayoutReady{false};
std::size_t LayoutSize = 0;
std::size_t LayoutAlign = alignof(std::max_align_t);
std::atomic<int> Constructions{0};
std::atomic<int> Destructions{0};

[[noreturn]] void fail(const LLVMStaticArenaVarV1 *Record, const char *Why) {
  std::fprintf(stderr, "static arena: %s", Why);
  if (Record)
    std::fprintf(stderr, " '%s'", Record->Name ? Record->Name : "<unnamed>");
  std::fputc('\n', stderr);
  std::abort();
}

bool isPowerOfTwo(std::uint64_t Value) {
  return Value && !(Value & (Value - 1));
}

std::size_t alignTo(std::size_t Value, std::size_t Alignment) {
  if (!isPowerOfTwo(Alignment) ||
      Value > std::numeric_limits<std::size_t>::max() - (Alignment - 1))
    fail(nullptr, "invalid allocation alignment");
  return (Value + Alignment - 1) & ~(Alignment - 1);
}

template <class Callback> void forEachRecord(Callback &&Visit) {
  std::uintptr_t BeginAddress =
      reinterpret_cast<std::uintptr_t>(recordBegin());
  std::uintptr_t EndAddress = reinterpret_cast<std::uintptr_t>(recordEnd());
  if ((BeginAddress == 0) != (EndAddress == 0) ||
      EndAddress < BeginAddress ||
      (EndAddress - BeginAddress) % sizeof(LLVMStaticArenaVarV1 *) != 0)
    fail(nullptr, "inconsistent record bounds");

  for (std::uintptr_t Address = BeginAddress; Address != EndAddress;
       Address += sizeof(LLVMStaticArenaVarV1 *)) {
    LLVMStaticArenaVarV1 *Record = nullptr;
    std::memcpy(&Record, reinterpret_cast<const void *>(Address),
                sizeof(Record));
    Visit(Record);
  }
}

void ensureLayout() {
  std::call_once(LayoutOnce, [] {
    std::vector<LLVMStaticArenaVarV1 *> Seen;
    std::uint64_t Cursor = 0;
    forEachRecord([&](LLVMStaticArenaVarV1 *Record) {
      if (!Record)
        return;
      if (std::find(Seen.begin(), Seen.end(), Record) != Seen.end())
        fail(Record, "duplicate record for");
      Seen.push_back(Record);
      if (Record->Offset != UINT64_MAX || !Record->Size ||
          !isPowerOfTwo(Record->Align) ||
          Record->Size > std::numeric_limits<std::size_t>::max() ||
          Record->Align > std::numeric_limits<std::size_t>::max())
        fail(Record, "invalid record for");
      if (Cursor > UINT64_MAX - (Record->Align - 1))
        fail(Record, "layout overflow for");
      Cursor = (Cursor + Record->Align - 1) & ~(Record->Align - 1);
      if (Cursor > UINT64_MAX - Record->Size)
        fail(Record, "layout overflow for");
      Record->Offset = Cursor;
      Cursor += Record->Size;
      LayoutAlign =
          std::max(LayoutAlign, static_cast<std::size_t>(Record->Align));
    });
    if (Cursor > std::numeric_limits<std::size_t>::max())
      fail(nullptr, "layout does not fit size_t");
    LayoutSize = static_cast<std::size_t>(Cursor);
    LayoutReady.store(true, std::memory_order_release);
  });
}

void runRange(const InitFn *Begin, const InitFn *End) {
  std::uintptr_t BeginAddress = reinterpret_cast<std::uintptr_t>(Begin);
  std::uintptr_t EndAddress = reinterpret_cast<std::uintptr_t>(End);
  if ((BeginAddress == 0) != (EndAddress == 0) ||
      EndAddress < BeginAddress ||
      (EndAddress - BeginAddress) % sizeof(InitFn) != 0)
    fail(nullptr, "inconsistent initializer bounds");
  for (std::uintptr_t Address = BeginAddress; Address != EndAddress;
       Address += sizeof(InitFn)) {
    InitFn Init = nullptr;
    std::memcpy(&Init, reinterpret_cast<const void *>(Address), sizeof(Init));
    if (Init)
      Init();
  }
}

} // namespace

namespace fixture {

struct Arena {
  enum class State { Live, Finalizing, Finalized } CurrentState = State::Live;
  void *Storage = nullptr;
  std::size_t AllocationSize = 0;
  std::size_t Attachments = 0;
  std::size_t Registered = 0;
  std::size_t Executed = 0;
  std::mutex Mutex;
  std::vector<std::pair<void (*)(void *), void *>> Destructors;
};

thread_local Arena *CurrentArena = nullptr;

Arena *createArena() {
  ensureLayout();
  auto *A = new Arena;
  A->AllocationSize = alignTo(std::max<std::size_t>(LayoutSize, 1), LayoutAlign);
#if defined(_WIN32)
  A->Storage = _aligned_malloc(A->AllocationSize, LayoutAlign);
#else
  A->Storage = std::aligned_alloc(LayoutAlign, A->AllocationSize);
#endif
  if (!A->Storage)
    fail(nullptr, "allocation failed");
  std::memset(A->Storage, 0, A->AllocationSize);
  forEachRecord([&](LLVMStaticArenaVarV1 *Record) {
    if (Record && Record->Templ)
      std::memcpy(static_cast<char *>(A->Storage) + Record->Offset,
                  Record->Templ, static_cast<std::size_t>(Record->Size));
  });
  return A;
}

void destroyArena(Arena *A) {
  {
    std::lock_guard<std::mutex> Lock(A->Mutex);
    if (A->CurrentState != Arena::State::Finalized || A->Attachments != 0 ||
        A->Registered != A->Executed)
      fail(nullptr, "release before finalization");
  }
#if defined(_WIN32)
  _aligned_free(A->Storage);
#else
  std::free(A->Storage);
#endif
  delete A;
}

Binding::Binding(Arena *A) : Bound(A), Previous(CurrentArena) {
  std::lock_guard<std::mutex> Lock(A->Mutex);
  if (A->CurrentState != Arena::State::Live)
    fail(nullptr, "binding an inactive arena");
  ++A->Attachments;
  CurrentArena = A;
}

Binding::~Binding() {
  if (CurrentArena != Bound)
    fail(nullptr, "bindings destroyed out of order");
  CurrentArena = Previous;
  std::lock_guard<std::mutex> Lock(Bound->Mutex);
  if (!Bound->Attachments)
    fail(nullptr, "attachment underflow");
  --Bound->Attachments;
}

void finalizeArena(Arena *A) {
  {
    std::lock_guard<std::mutex> Lock(A->Mutex);
    if (CurrentArena != A || A->Attachments != 1 ||
        A->CurrentState != Arena::State::Live)
      fail(nullptr, "arena is not owner-bound for finalization");
    A->CurrentState = Arena::State::Finalizing;
  }
  for (;;) {
    std::pair<void (*)(void *), void *> Callback;
    {
      std::lock_guard<std::mutex> Lock(A->Mutex);
      if (A->Destructors.empty()) {
        A->CurrentState = Arena::State::Finalized;
        return;
      }
      Callback = A->Destructors.back();
      A->Destructors.pop_back();
    }
    Callback.first(Callback.second);
    {
      std::lock_guard<std::mutex> Lock(A->Mutex);
      ++A->Executed;
    }
  }
}

std::size_t registeredDestructors(Arena *A) { return A->Registered; }
std::size_t executedDestructors(Arena *A) { return A->Executed; }

void runCommonInitializers() {
  runRange(llvmi_e2e_commonBegin, llvmi_e2e_commonEnd);
}
void runAInitializers() { runRange(llvmi_e2e_aBegin, llvmi_e2e_aEnd); }
void runBInitializers() { runRange(llvmi_e2e_bBegin, llvmi_e2e_bEnd); }

void noteConstruction() {
  Constructions.fetch_add(1, std::memory_order_relaxed);
}
void noteDestruction() {
  Destructions.fetch_add(1, std::memory_order_relaxed);
}
int constructionCount() { return Constructions.load(std::memory_order_relaxed); }
int destructionCount() { return Destructions.load(std::memory_order_relaxed); }

} // namespace fixture

extern "C" void *__llvm_arena_addr_v1(const LLVMStaticArenaVarV1 *Record) {
  if (!LayoutReady.load(std::memory_order_acquire))
    fail(Record, "layout is not ready while accessing");
  if (!Record)
    fail(nullptr, "null record");
  fixture::Arena *A = fixture::CurrentArena;
  if (!A)
    fail(Record, "no invocation is attached while accessing");
  if (A->CurrentState != fixture::Arena::State::Live &&
      A->CurrentState != fixture::Arena::State::Finalizing)
    fail(Record, "inactive arena while accessing");
  if (Record->Offset == UINT64_MAX || Record->Offset > LayoutSize ||
      Record->Size > LayoutSize - static_cast<std::size_t>(Record->Offset))
    fail(Record, "record is out of bounds for");
  return static_cast<char *>(A->Storage) + Record->Offset;
}

extern "C" int __llvm_arena_atexit_v1(void (*Destroy)(void *), void *Object) {
  fixture::Arena *A = fixture::CurrentArena;
  if (!A || !Destroy)
    fail(nullptr, "invalid destructor registration");
  std::lock_guard<std::mutex> Lock(A->Mutex);
  if (A->CurrentState != fixture::Arena::State::Live &&
      A->CurrentState != fixture::Arena::State::Finalizing)
    fail(nullptr, "destructor registration on inactive arena");
  A->Destructors.emplace_back(Destroy, Object);
  ++A->Registered;
  return 0;
}

//--- common.cpp
#include "fixture.h"

#include <cassert>
#include <cstdint>

namespace fixture {

Registry GlobalRegistry;
static Opt<std::string> *CPUView;
int CommonCounter;
int TemplateValue = 42;
struct alignas(64) AlignedValue {
  int Value;
};
AlignedValue AlignmentProbe{};

Registry::Registry() { noteConstruction(); }
Registry::~Registry() { noteDestruction(); }

void Registry::add(const std::string &Name, const void *Option) {
  if (!Options.emplace(Name, Option).second)
    ++Collisions;
}

void Registry::remove(const std::string &Name, const void *Option) {
  auto It = Options.find(Name);
  assert(It != Options.end() && It->second == Option);
  Options.erase(It);
}

Registry &registry() { return GlobalRegistry; }

static Opt<std::string> &cpuOption() {
  static Opt<std::string> CPU("mcpu");
  return CPU;
}

void setCPU(const char *CPU) {
  CPUView = &cpuOption();
  *CPUView = std::string(CPU);
}

std::string getCPU() { return static_cast<const std::string &>(*CPUView); }
int commonTemplateValue() { return TemplateValue; }
void setCommonTemplateValue(int Value) { TemplateValue = Value; }
void *commonAddress() { return &CommonCounter; }
bool commonIsOverAligned() {
  return reinterpret_cast<std::uintptr_t>(&AlignmentProbe) % 64 == 0;
}
int registryCollisions() { return GlobalRegistry.Collisions; }

} // namespace fixture

//--- tool-a.cpp
#include "fixture.h"

namespace fixture {

inline int SharedInlineArenaValue;
int ToolAVerboseLocation;
Opt<int> ToolAVerbose("verbose", &ToolAVerboseLocation);
Label ToolALabel("A-unset");
Counted ToolALifetime;
int ToolARunCount;

void runToolA(const char *CPU) {
  ToolAVerbose = 11;
  ToolALabel.set("A-ran");
  setCPU(CPU);
  ++ToolARunCount;
}

void *toolAInlineAddress() { return &SharedInlineArenaValue; }
int toolARuns() { return ToolARunCount; }
int toolAVerbose() { return ToolAVerboseLocation; }
std::string toolALabel() { return ToolALabel.get(); }

} // namespace fixture

//--- tool-b.cpp
#include "fixture.h"

namespace fixture {

inline int SharedInlineArenaValue;
int ToolBVerboseLocation;
Opt<int> ToolBVerbose("verbose", &ToolBVerboseLocation);
Label ToolBLabel("B-unset");
Counted ToolBLifetime;
int ToolBRunCount;

void runToolB(const char *CPU) {
  ToolBVerbose = 22;
  ToolBLabel.set("B-ran");
  setCPU(CPU);
  ++ToolBRunCount;
}

void *toolBInlineAddress() { return &SharedInlineArenaValue; }
int toolBRuns() { return ToolBRunCount; }
int toolBVerbose() { return ToolBVerboseLocation; }
std::string toolBLabel() { return ToolBLabel.get(); }

} // namespace fixture

//--- main.cpp
#include "fixture.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace fixture;

static void finish(Arena *A) {
  finalizeArena(A);
  assert(registeredDestructors(A) == 5);
  assert(executedDestructors(A) == 5);
}

int main(int Argc, char **Argv) {
  if (Argc == 2 && std::strcmp(Argv[1], "--unbound") == 0) {
    (void)createArena();
    (void)commonAddress();
    return 1;
  }

  assert(constructionCount() == 0);
  std::puts("constructed-before-main=0");

  Arena *A = createArena();
  Arena *B = createArena();
  void *AddressA = nullptr;
  void *AddressB = nullptr;

  std::thread ThreadA([&] {
    Binding Bind(A);
    runCommonInitializers();
    runAInitializers();
    runToolA("znver2");
    assert(toolAInlineAddress() == toolBInlineAddress());
    AddressA = commonAddress();
    assert(toolARuns() == 1 && toolAVerbose() == 11);
    assert(toolALabel() == "A-ran" && getCPU() == "znver2");
    assert(commonTemplateValue() == 42 && commonIsOverAligned());
    setCommonTemplateValue(101);
    assert(registryCollisions() == 0);
  });
  std::thread ThreadB([&] {
    Binding Bind(B);
    runCommonInitializers();
    runBInitializers();
    runToolB("skylake");
    AddressB = commonAddress();
    assert(toolBRuns() == 1 && toolBVerbose() == 22);
    assert(toolBLabel() == "B-ran" && getCPU() == "skylake");
    assert(commonTemplateValue() == 42 && commonIsOverAligned());
    setCommonTemplateValue(202);
    assert(registryCollisions() == 0);
  });
  ThreadA.join();
  ThreadB.join();

  // Keep this OS thread alive while it visits A, nests B, restores A, and
  // later initializes newly created arenas. On the Microsoft ABI each call to
  // setCPU() passes through the per-arena local-static guard while the real
  // _Init_thread_epoch remains per-thread. A stale process guard or an epoch
  // that incorrectly suppresses a fresh arena construction fails here or in
  // the A2/re-created-A checks below.
  {
    Binding BindA(A);
    setCPU("znver2-main-thread");
    assert(getCPU() == "znver2-main-thread");
    {
      Binding BindB(B);
      setCPU("skylake-main-thread");
      assert(getCPU() == "skylake-main-thread");
    }
    assert(getCPU() == "znver2-main-thread");
    setCPU("znver2");
  }
  {
    Binding BindB(B);
    setCPU("skylake");
  }

  assert(AddressA != AddressB);
  std::puts("same-name-options=ok");
  std::puts("concurrent-independent=ok");
  std::puts("same-global-address=distinct");
  std::puts("cached-pointer=ok");
  std::puts("comdat-dedup=ok");
  std::puts("persistent-thread-epoch=ok");

  // A second instance of the same tool can coexist with A and starts from the
  // compiler-emitted zero/template state.
  Arena *A2 = createArena();
  {
    Binding Bind(A2);
    runCommonInitializers();
    runAInitializers();
    assert(toolARuns() == 0 && commonTemplateValue() == 42);
    assert(commonAddress() != AddressA);
    runToolA("znver4");
    assert(getCPU() == "znver4");
    finish(A2);
  }
  destroyArena(A2);
  assert(destructionCount() == 5);

  {
    Binding Bind(A);
    assert(toolARuns() == 1 && getCPU() == "znver2");
    assert(commonTemplateValue() == 101);
    finish(A);
  }
  destroyArena(A);
  {
    Binding Bind(B);
    assert(toolBRuns() == 1 && getCPU() == "skylake");
    assert(commonTemplateValue() == 202);
    finish(B);
  }
  destroyArena(B);

  A = createArena();
  {
    Binding Bind(A);
    runCommonInitializers();
    runAInitializers();
    assert(toolARuns() == 0 && commonTemplateValue() == 42);
    runToolA("znver5");
    assert(getCPU() == "znver5");
    finish(A);
  }
  destroyArena(A);
  assert(constructionCount() == 20);
  assert(destructionCount() == 20);

  std::puts("recreation-and-dtors=ok");
  std::puts("static-arena-e2e=passed");
  return 0;
}
