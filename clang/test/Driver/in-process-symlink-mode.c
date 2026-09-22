// REQUIRES: llvm-driver
// UNSUPPORTED: system-windows

// Preserve a mode-selecting intermediate name across a symlink chain. The
// outer name remains argv[0], while ToolContext identifies clang-cl as the
// selected folded tool.
// RUN: rm -rf %t
// RUN: mkdir %t
// RUN: ln -s %clang %t/clang-cl
// RUN: ln -s %t/clang-cl %t/arbitrary-clang-name
// RUN: %t/arbitrary-clang-name /help | FileCheck %s

// An exact outer alias takes precedence over the intermediate alias.
// RUN: ln -s %t/clang-cl %t/clang
// RUN: %t/clang --help | FileCheck %s --check-prefix=CLANG

// Target and version decorations are also authoritative outer spellings.
// RUN: ln -s %t/clang-cl %t/x86_64-unknown-linux-gnu-clang++
// RUN: %t/x86_64-unknown-linux-gnu-clang++ -### -c %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CXX
// RUN: ln -s %t/clang %t/clang-cl-22
// RUN: %t/clang-cl-22 -### -- %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CL

// CHECK: CL.EXE COMPATIBILITY OPTIONS:
// CLANG: OVERVIEW: clang LLVM compiler
// CXX: "-x" "c++"
// CL: "-fdiagnostics-format" "msvc"
