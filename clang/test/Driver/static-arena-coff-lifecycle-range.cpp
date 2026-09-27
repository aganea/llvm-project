// Run three COFF lifecycle ranges that share the .foo output section. Each
// range must invoke only its own dynamic initializer, including the IDs with
// no underscore and with a trailing underscore.
//
// REQUIRES: system-windows, x86-registered-target, lld
// RUN: split-file %s %t
// RUN: %clang --target=x86_64-pc-windows-msvc -std=c++20 -c \
// RUN:   -Xclang -fstatic-arena=foo -Xclang -fstatic-arena-list=%t/arena.list \
// RUN:   -o %t/foo.obj %t/foo.cpp
// RUN: %clang --target=x86_64-pc-windows-msvc -std=c++20 -c \
// RUN:   -Xclang -fstatic-arena=foo_ -Xclang -fstatic-arena-list=%t/arena.list \
// RUN:   -o %t/foo-under.obj %t/foo-under.cpp
// RUN: %clang --target=x86_64-pc-windows-msvc -std=c++20 -c \
// RUN:   -Xclang -fstatic-arena=foo_bar -Xclang -fstatic-arena-list=%t/arena.list \
// RUN:   -o %t/foo-bar.obj %t/foo-bar.cpp
// RUN: %clang --target=x86_64-pc-windows-msvc -std=c++20 -c \
// RUN:   -o %t/host.obj %t/host.cpp
// RUN: lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib \
// RUN:   /out:%t/test.exe %t/host.obj %t/foo.obj %t/foo-under.obj \
// RUN:   %t/foo-bar.obj
// RUN: %t/test.exe

//--- arena.list
[arena]
name:arena_foo
name:arena_foo_under
name:arena_foo_bar

//--- foo.cpp
extern "C" int mark_foo();
int arena_foo = mark_foo();

//--- foo-under.cpp
extern "C" int mark_foo_under();
int arena_foo_under = mark_foo_under();

//--- foo-bar.cpp
extern "C" int mark_foo_bar();
int arena_foo_bar = mark_foo_bar();

//--- host.cpp
using InitFn = void (*)();
using UIntPtr = __UINTPTR_TYPE__;

#define RANGE(Name, Prefix)                                                \
  __attribute__((section(Prefix "$a"), used)) static InitFn const         \
      Name##Start = nullptr;                                               \
  __attribute__((section(Prefix "$z"), used)) static InitFn const         \
      Name##End = nullptr

RANGE(Foo, ".foo$$");
RANGE(FooUnder, ".foo$");
RANGE(FooBar, ".foo$bar");

static int FooCount;
static int FooUnderCount;
static int FooBarCount;
static int ArenaStorage;

extern "C" int mark_foo() { return ++FooCount; }
extern "C" int mark_foo_under() { return ++FooUnderCount; }
extern "C" int mark_foo_bar() { return ++FooBarCount; }
extern "C" void *__llvm_arena_addr_v1(const void *) {
  return &ArenaStorage;
}

static bool runRange(InitFn const *Start, InitFn const *End) {
  UIntPtr Begin = reinterpret_cast<UIntPtr>(Start) + sizeof(InitFn);
  UIntPtr EndAddress = reinterpret_cast<UIntPtr>(End);
  if (EndAddress < Begin || (EndAddress - Begin) % sizeof(InitFn) != 0)
    return false;

  for (UIntPtr Address = Begin; Address != EndAddress;
       Address += sizeof(InitFn)) {
    InitFn Init = nullptr;
    // Copy one section entry without indexing across distinct C++ objects.
    auto *Dst = reinterpret_cast<unsigned char *>(&Init);
    auto *Src = reinterpret_cast<const unsigned char *>(Address);
    for (unsigned I = 0; I < sizeof(Init); ++I)
      Dst[I] = Src[I];
    if (Init)
      Init();
  }
  return true;
}

extern "C" int mainCRTStartup() {
  if (FooCount || FooUnderCount || FooBarCount)
    return 1;
  if (!runRange(&FooStart, &FooEnd) || FooCount != 1 || FooUnderCount ||
      FooBarCount)
    return 2;
  if (!runRange(&FooUnderStart, &FooUnderEnd) || FooCount != 1 ||
      FooUnderCount != 1 || FooBarCount)
    return 3;
  if (!runRange(&FooBarStart, &FooBarEnd) || FooCount != 1 ||
      FooUnderCount != 1 || FooBarCount != 1)
    return 4;
  return 0;
}
