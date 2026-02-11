# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-unknown -o %t.o %s

## --disable-free is accepted and the link succeeds.
# RUN: wasm-ld --disable-free -e _start -o /dev/null %t.o

## --no-disable-free is accepted and the link succeeds.
# RUN: wasm-ld --no-disable-free -e _start -o /dev/null %t.o

## Last flag wins.
# RUN: wasm-ld --disable-free --no-disable-free -e _start -o /dev/null %t.o
# RUN: wasm-ld --no-disable-free --disable-free -e _start -o /dev/null %t.o

.globl _start
_start:
  .functype _start () -> ()
  end_function
