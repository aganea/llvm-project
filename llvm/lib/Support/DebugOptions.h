//===-- DebugOptions.h - Global Command line opt for libSupport  *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the entry point to initialize the options registered on the
// command line for libSupport, this is internal to libSupport.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_DEBUGOPTIONS_H
#define LLVM_SUPPORT_DEBUGOPTIONS_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"

namespace llvm {

namespace cl {
class Option;
} // namespace cl

// These are invoked internally before parsing command line options.
// This enables lazy-initialization of all the globals in libSupport, instead
// of eagerly loading everything on program startup.
void initDebugCounterOptions();
void initGraphWriterOptions();
void initSignalsOptions();
/// Returns true for command-line controls of process-wide crash/signal state.
/// Invocation contexts own equivalent options, so these process-default
/// objects must not also be inherited into them.
bool isProcessWideSignalsOption(const cl::Option *Option);
/// Reads the signal settings for the explicitly bound invocation without
/// taking locks or lazily constructing command-line state. Returns false when
/// this thread has no explicit context, in which case Signals.cpp uses its
/// process-default settings.
LLVM_ABI bool getCurrentSignalOptions(bool &DisableSymbolication,
                                      StringRef &CrashDiagnosticsDirectory);
void initStatisticOptions();
void initTimerOptions();
void initWithColorOptions();
void initDebugOptions();
void initRandomSeedOptions();

} // namespace llvm

#endif // LLVM_SUPPORT_DEBUGOPTIONS_H
