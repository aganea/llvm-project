// REQUIRES: lld, llvm-driver, webassembly-registered-target
//
// Put a valid but deliberately wrong executable at the selected wasm-ld path.
// If the driver spawns it, FileCheck rejects the linker arguments. Registered
// in-process dispatch instead calls the embedded wasm-ld and produces Wasm.
// RUN: rm -rf %t
// RUN: mkdir -p %t
// Use both executable spellings so -B finds the fake on every host.
// RUN: cp FileCheck %t/wasm-ld
// RUN: cp FileCheck %t/wasm-ld.exe
// RUN: %clang --target=wasm32-unknown-unknown -nostdlib -fuse-ld=lld \
// RUN:   -B%t -Wl,--no-entry,--export=answer %s -o %t/out.wasm
// RUN: llvm-readobj --file-headers %t/out.wasm | FileCheck %s
// An explicit linker path retains the subprocess contract. The WebAssembly
// toolchain does not consume --ld-path itself, so -B makes either path reach
// the deliberately invalid executable when in-process interception is off.
// RUN: not %clang --target=wasm32-unknown-unknown -nostdlib -fuse-ld=lld \
// RUN:   --ld-path=%t/wasm-ld%exeext -B%t -Wl,--no-entry %s -o %t/ld-path.wasm
// RUN: not %clang --target=wasm32-unknown-unknown -nostdlib \
// RUN:   -fuse-ld=%t/wasm-ld%exeext -Wl,--no-entry %s -o %t/fuse-path.wasm
//
// CHECK: Format: WASM
// CHECK-NEXT: Arch: wasm32

int answer(void) { return 42; }
