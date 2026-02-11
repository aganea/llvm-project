# REQUIRES: x86

# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %s -o %t.o

## --disable-free is accepted and the link succeeds.
# RUN: ld.lld --disable-free %t.o -o /dev/null

## --no-disable-free is accepted and the link succeeds.
# RUN: ld.lld --no-disable-free %t.o -o /dev/null

## Last flag wins.
# RUN: ld.lld --disable-free --no-disable-free %t.o -o /dev/null
# RUN: ld.lld --no-disable-free --disable-free %t.o -o /dev/null

.globl _start
_start:
  ret
