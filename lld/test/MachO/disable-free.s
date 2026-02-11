# REQUIRES: x86

# RUN: llvm-mc -filetype=obj -triple=x86_64-apple-darwin %s -o %t.o

## --disable-free is accepted and the link succeeds.
# RUN: %lld --disable-free -lSystem %t.o -o /dev/null

## --no-disable-free is accepted and the link succeeds.
# RUN: %lld --no-disable-free -lSystem %t.o -o /dev/null

## Last flag wins.
# RUN: %lld --disable-free --no-disable-free -lSystem %t.o -o /dev/null
# RUN: %lld --no-disable-free --disable-free -lSystem %t.o -o /dev/null

.globl _main
_main:
  ret
