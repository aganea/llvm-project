// Verify that every externally visible part of the static-arena ABI is
// versioned independently.  Use a fixed ELF target so these negative links
// have the same diagnostics on every host.
//
// REQUIRES: x86-registered-target, lld
// RUN: split-file %s %t
// RUN: %clang -target x86_64-unknown-linux-gnu -c \
// RUN:   -Xclang -fstatic-arena=arena_test \
// RUN:   -Xclang -fstatic-arena-list=%t/arena.list \
// RUN:   -o %t/reference.o %t/reference.c
// RUN: %clang -target x86_64-unknown-linux-gnu -c \
// RUN:   -o %t/resolver-v2.o %t/resolver-v2.c
// RUN: not %clang -target x86_64-unknown-linux-gnu -fuse-ld=lld -nostdlib \
// RUN:   -Wl,-e,read_arena_value -o %t/resolver-mismatch \
// RUN:   %t/reference.o %t/resolver-v2.o 2>&1 \
// RUN:   | FileCheck %s --check-prefix=RESOLVER
// RUN: %clang -target x86_64-unknown-linux-gnu -c \
// RUN:   -o %t/record-v2.o %t/record-v2.c
// RUN: not %clang -target x86_64-unknown-linux-gnu -fuse-ld=lld -nostdlib \
// RUN:   -Wl,-e,read_arena_value -o %t/record-mismatch \
// RUN:   %t/reference.o %t/record-v2.o 2>&1 \
// RUN:   | FileCheck %s --check-prefix=RECORD
// RUN: %clang -target x86_64-unknown-linux-gnu -c \
// RUN:   -o %t/ordinary-definition.o %t/ordinary-definition.c
// RUN: not %clang -target x86_64-unknown-linux-gnu -fuse-ld=lld -nostdlib \
// RUN:   -Wl,-e,read_arena_value -o %t/mixed-mode \
// RUN:   %t/reference.o %t/ordinary-definition.o 2>&1 \
// RUN:   | FileCheck %s --check-prefix=MIXED
// RUN: %clang -target x86_64-pc-windows-msvc -c -Xclang -fstatic-arena=arena_test -Xclang -fstatic-arena-list=%t/arena.list -o %t/coff-retention.obj %t/coff-retention.c
// RUN: %clang -target x86_64-pc-windows-msvc -c -o %t/coff-resolver.obj %t/coff-resolver.c
// RUN: %clang -target x86_64-pc-windows-msvc -fuse-ld=lld -nostdlib -Wl,/entry:read_arena_value,/subsystem:console,/opt:ref -o %t/coff-retention.exe %t/coff-retention.obj %t/coff-resolver.obj
// RUN: llvm-readobj --sections %t/coff-retention.exe | FileCheck %s --check-prefix=COFF-REF

// RESOLVER: error: undefined symbol: __llvm_arena_addr_v1
// RECORD: error: undefined symbol: __llvm_arena_var_v1.arena_value
// MIXED: error: undefined symbol: __llvm_arena_var_v1.arena_value
// COFF-REF:      Name: .llvma
// COFF-REF-NEXT: VirtualSize: 0x8

//--- arena.list
[arena]
global:arena_value

//--- reference.c
extern int arena_value;

int read_arena_value(void) { return arena_value; }

//--- resolver-v2.c
struct ArenaRecord {
  unsigned long long Offset;
  unsigned long long Size;
  unsigned long long Alignment;
  void *Template;
  void *Name;
};

struct ArenaRecord v1_record
    __asm__("__llvm_arena_var_v1.arena_value") = {0, 4, 4, 0, 0};

void *__llvm_arena_addr_v2(void *Record) { return Record; }

//--- record-v2.c
struct ArenaRecord {
  unsigned long long Offset;
  unsigned long long Size;
  unsigned long long Alignment;
  void *Template;
  void *Name;
};

struct ArenaRecord v2_record
    __asm__("__llvm_arena_var_v2.arena_value") = {0, 4, 4, 0, 0};

void *__llvm_arena_addr_v1(void *Record) { return Record; }
void *__llvm_arena_addr_v2(void *Record) { return Record; }

//--- ordinary-definition.c
int arena_value;

void *__llvm_arena_addr_v1(void *Record) { return Record; }

//--- coff-retention.c
int arena_value;

int read_arena_value(void) { return arena_value; }

//--- coff-resolver.c
void *__llvm_arena_addr_v1(void *Record) { return Record; }
