// Exercise Clang-produced arena records through full and ThinLTO. The fixture
// also has duplicate inline definitions, so its runtime duplicate-record check
// proves that the record and pointer-entry COMDATs coalesce before layout.
//
// REQUIRES: lld, native, thread_support
// UNSUPPORTED: system-darwin, system-aix, system-zos
// RUN: rm -rf %t && split-file %S/../CodeGenCXX/static-arena-e2e.cpp %t
// RUN: %clangxx -std=c++20 -O2 -ffunction-sections -fdata-sections \
// RUN:   -flto=full -c %t/runtime.cpp -o %t/runtime.full.o
// RUN: %clangxx -std=c++20 -O2 -ffunction-sections -fdata-sections \
// RUN:   -flto=full -Xclang -fstatic-arena=llvmi_e2e_common \
// RUN:   -Xclang -fstatic-arena-list=%t/arena.list \
// RUN:   -c %t/common.cpp -o %t/common.full.o
// RUN: %clangxx -std=c++20 -O2 -ffunction-sections -fdata-sections \
// RUN:   -flto=full -Xclang -fstatic-arena=llvmi_e2e_a \
// RUN:   -Xclang -fstatic-arena-list=%t/arena.list \
// RUN:   -c %t/tool-a.cpp -o %t/tool-a.full.o
// RUN: %clangxx -std=c++20 -O2 -ffunction-sections -fdata-sections \
// RUN:   -flto=full -Xclang -fstatic-arena=llvmi_e2e_b \
// RUN:   -Xclang -fstatic-arena-list=%t/arena.list \
// RUN:   -c %t/tool-b.cpp -o %t/tool-b.full.o
// RUN: %clangxx -std=c++20 -O2 -ffunction-sections -fdata-sections \
// RUN:   -flto=full -c %t/main.cpp -o %t/main.full.o
// RUN: %clangxx -std=c++20 -O2 -flto=full -fuse-ld=lld \
// RUN:   %if !system-windows %{ -pthread %} \
// RUN:   %t/runtime.full.o %t/common.full.o %t/tool-a.full.o \
// RUN:   %t/tool-b.full.o %t/main.full.o \
// RUN:   %if system-windows %{ -Wl,/OPT:REF %} \
// RUN:   %else %{ -Wl,--gc-sections %} -o %t/e2e-full.exe
// RUN: %t/e2e-full.exe | FileCheck %S/../CodeGenCXX/static-arena-e2e.cpp
// RUN: not --crash %t/e2e-full.exe --unbound 2>&1 \
// RUN:   | FileCheck --check-prefix=TRAP \
// RUN:       %S/../CodeGenCXX/static-arena-e2e.cpp
// RUN: %clangxx -std=c++20 -O2 -ffunction-sections -fdata-sections \
// RUN:   -flto=thin -c %t/runtime.cpp -o %t/runtime.thin.o
// RUN: %clangxx -std=c++20 -O2 -ffunction-sections -fdata-sections \
// RUN:   -flto=thin -Xclang -fstatic-arena=llvmi_e2e_common \
// RUN:   -Xclang -fstatic-arena-list=%t/arena.list \
// RUN:   -c %t/common.cpp -o %t/common.thin.o
// RUN: %clangxx -std=c++20 -O2 -ffunction-sections -fdata-sections \
// RUN:   -flto=thin -Xclang -fstatic-arena=llvmi_e2e_a \
// RUN:   -Xclang -fstatic-arena-list=%t/arena.list \
// RUN:   -c %t/tool-a.cpp -o %t/tool-a.thin.o
// RUN: %clangxx -std=c++20 -O2 -ffunction-sections -fdata-sections \
// RUN:   -flto=thin -Xclang -fstatic-arena=llvmi_e2e_b \
// RUN:   -Xclang -fstatic-arena-list=%t/arena.list \
// RUN:   -c %t/tool-b.cpp -o %t/tool-b.thin.o
// RUN: %clangxx -std=c++20 -O2 -ffunction-sections -fdata-sections \
// RUN:   -flto=thin -c %t/main.cpp -o %t/main.thin.o
// RUN: %clangxx -std=c++20 -O2 -flto=thin -fuse-ld=lld \
// RUN:   %if !system-windows %{ -pthread %} \
// RUN:   %t/runtime.thin.o %t/common.thin.o %t/tool-a.thin.o \
// RUN:   %t/tool-b.thin.o %t/main.thin.o \
// RUN:   %if system-windows %{ -Wl,/OPT:REF %} \
// RUN:   %else %{ -Wl,--gc-sections %} -o %t/e2e-thin.exe
// RUN: %t/e2e-thin.exe | FileCheck %S/../CodeGenCXX/static-arena-e2e.cpp
// RUN: not --crash %t/e2e-thin.exe --unbound 2>&1 \
// RUN:   | FileCheck --check-prefix=TRAP \
// RUN:       %S/../CodeGenCXX/static-arena-e2e.cpp
