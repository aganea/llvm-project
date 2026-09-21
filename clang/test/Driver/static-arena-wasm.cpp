// Verify the Wasm static-arena link contract without requiring an Emscripten
// sysroot. In particular, --gc-sections must retain both the arena record
// entries and the deferred lifecycle entry, and wasm-ld must synthesize usable
// bounds for their C-identifier section names.
//
// REQUIRES: lld, webassembly-registered-target
// RUN: rm -rf %t && split-file %s %t
// RUN: %clangxx --target=wasm32-unknown-emscripten -std=c++20 -O2 \
// RUN:   -ffunction-sections -fdata-sections \
// RUN:   -Xclang -fstatic-arena=llvmi_wasm_test \
// RUN:   -Xclang -fstatic-arena-list=%t/arena.list \
// RUN:   -c %t/producer.cpp -o %t/producer.o
// RUN: %clangxx --target=wasm32-unknown-emscripten -std=c++20 -O2 \
// RUN:   -ffunction-sections -fdata-sections \
// RUN:   -c %t/ranges.cpp -o %t/ranges.o
// RUN: %clangxx --target=wasm32-unknown-emscripten -nostdlib -fuse-ld=lld \
// RUN:   -Wl,--no-entry,--gc-sections \
// RUN:   -Wl,--export=record_begin,--export=record_end \
// RUN:   -Wl,--export=lifecycle_begin,--export=lifecycle_end \
// RUN:   -Wl,--export=missing_begin,--export=missing_end \
// RUN:   %t/producer.o %t/ranges.o -o %t/test.wasm
// RUN: llvm-objdump -d --no-show-raw-insn %t/test.wasm \
// RUN:   | FileCheck %s --check-prefix=ASM
// RUN: obj2yaml %t/test.wasm | FileCheck %s --check-prefix=YAML
// RUN: %clangxx --target=wasm32-unknown-emscripten -std=c++20 -O2 \
// RUN:   -flto=thin -ffunction-sections -fdata-sections \
// RUN:   -Xclang -fstatic-arena=llvmi_wasm_test \
// RUN:   -Xclang -fstatic-arena-list=%t/arena.list \
// RUN:   -c %t/producer.cpp -o %t/producer.thin.o
// RUN: %clangxx --target=wasm32-unknown-emscripten -std=c++20 -O2 \
// RUN:   -flto=thin -ffunction-sections -fdata-sections \
// RUN:   -c %t/ranges.cpp -o %t/ranges.thin.o
// RUN: %clangxx --target=wasm32-unknown-emscripten -nostdlib -fuse-ld=lld \
// RUN:   -flto=thin -Wl,--no-entry,--gc-sections \
// RUN:   -Wl,--export=record_begin,--export=record_end \
// RUN:   -Wl,--export=lifecycle_begin,--export=lifecycle_end \
// RUN:   -Wl,--export=missing_begin,--export=missing_end \
// RUN:   %t/producer.thin.o %t/ranges.thin.o -o %t/test.thin.wasm
// RUN: llvm-objdump -d --no-show-raw-insn %t/test.thin.wasm \
// RUN:   | FileCheck %s --check-prefix=ASM
// RUN: obj2yaml %t/test.thin.wasm | FileCheck %s --check-prefix=YAML
//
// ASM-LABEL: <record_begin>:
// ASM: i32.const [[#RECORD_BEGIN:]]
// ASM-LABEL: <record_end>:
// ASM: i32.const [[#RECORD_BEGIN+8]]
// ASM-LABEL: <lifecycle_begin>:
// ASM: i32.const [[#LIFECYCLE_BEGIN:]]
// ASM-LABEL: <lifecycle_end>:
// ASM: i32.const [[#LIFECYCLE_BEGIN+4]]
// ASM-LABEL: <missing_begin>:
// ASM: i32.const 0
// ASM-LABEL: <missing_end>:
// ASM: i32.const 0
// YAML: DataSegmentNames:
// YAML-DAG: Name: llvma_v1
// YAML-DAG: Name: llvmi_wasm_test

//--- arena.list
[arena]
global:arena_*

//--- producer.cpp
struct Dynamic {
  Dynamic();
  int Value;
};

Dynamic::Dynamic() : Value(7) {}

extern "C" {
int arena_zero;
Dynamic arena_dynamic;
int ordinary_unused = 1;
}

//--- ranges.cpp
struct ArenaRecord;
using InitFn = void (*)();
using UIntPtr = __UINTPTR_TYPE__;

extern "C" {
extern ArenaRecord *const __start_llvma_v1[] __attribute__((weak));
extern ArenaRecord *const __stop_llvma_v1[] __attribute__((weak));
extern InitFn const __start_llvmi_wasm_test[] __attribute__((weak));
extern InitFn const __stop_llvmi_wasm_test[] __attribute__((weak));
extern InitFn const __start_llvmi_missing[] __attribute__((weak));
extern InitFn const __stop_llvmi_missing[] __attribute__((weak));

UIntPtr record_begin() {
  return reinterpret_cast<UIntPtr>(__start_llvma_v1);
}
UIntPtr record_end() { return reinterpret_cast<UIntPtr>(__stop_llvma_v1); }
UIntPtr lifecycle_begin() {
  return reinterpret_cast<UIntPtr>(__start_llvmi_wasm_test);
}
UIntPtr lifecycle_end() {
  return reinterpret_cast<UIntPtr>(__stop_llvmi_wasm_test);
}
UIntPtr missing_begin() {
  return reinterpret_cast<UIntPtr>(__start_llvmi_missing);
}
UIntPtr missing_end() {
  return reinterpret_cast<UIntPtr>(__stop_llvmi_missing);
}

void *__llvm_arena_addr_v1(void *Record) { return Record; }
}
