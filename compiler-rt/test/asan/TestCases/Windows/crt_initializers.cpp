// RUN: %clang_cl_asan %Od %s %Fe%t
// RUN: %run %t | FileCheck %s

// This is a test for http://code.google.com/p/address-sanitizer/issues/detail?id=305

#include <stdio.h>

typedef void (*FPTR)();

// __xi_a and __xi_z are defined in VC/crt/src/crt0dat.c
// and are located in .CRT$XIA and .CRT$XIZ respectively.
extern "C" FPTR __xi_a, __xi_z;

int main() {
  unsigned count = 0;

  // Iterate through CRT initializers.
  for (FPTR* it = &__xi_a; it < &__xi_z; ++it) {
    if (*it)
      count++;
  }

  printf("Number of nonzero CRT initializers: %u\n", count);
// CHECK: Number of nonzero CRT initializers
}

// Entries in .CRT$XI* are invoked by the CRT via _initterm_e as
// int(__cdecl*)(void); a non-zero return aborts startup and becomes the
// process exit code. call_me_maybe must therefore return 0 -- a void
// function here leaves an arbitrary value in eax/rax that _initterm_e can
// (and does, with some codegen) interpret as a fatal init failure.
extern "C" int __cdecl call_me_maybe() { return 0; }

#pragma data_seg(".CRT$XIB")
// Add an initializer that shouldn't get its own redzone.
FPTR run_on_startup = reinterpret_cast<FPTR>(call_me_maybe);
