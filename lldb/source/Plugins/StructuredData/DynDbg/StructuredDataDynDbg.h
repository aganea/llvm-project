//===-- StructuredDataDynDbg.h ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// LLDB plugin for on-demand deoptimization of C++ code on COFF/CodeView
// (Windows). Reads pre-optimization bitcode from .dyndbg sections in .obj
// files, extracts a single function, codegens at -O0, loads the result into
// the debuggee, and patches the optimized function entry with a JMP.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_STRUCTUREDDATA_DYNDBG_STRUCTUREDDATADYNDBG_H
#define LLDB_SOURCE_PLUGINS_STRUCTUREDDATA_DYNDBG_STRUCTUREDDATADYNDBG_H

#include "lldb/Target/StructuredDataPlugin.h"
#include "lldb/Utility/Status.h"

namespace lldb_private {

class StructuredDataDynDbg : public StructuredDataPlugin {
public:
  static void Initialize();
  static void Terminate();

  static llvm::StringRef GetStaticPluginName() { return "dyndbg"; }

  llvm::StringRef GetPluginName() override { return GetStaticPluginName(); }

  bool SupportsStructuredDataType(llvm::StringRef type_name) override {
    return false;
  }

  void HandleArrivalOfStructuredData(
      Process &process, llvm::StringRef type_name,
      const StructuredData::ObjectSP &object_sp) override {}

  Status GetDescription(const StructuredData::ObjectSP &object_sp,
                        lldb_private::Stream &stream) override {
    return Status();
  }

  bool GetEnabled(llvm::StringRef type_name) const override { return false; }

  ~StructuredDataDynDbg() override = default;

private:
  StructuredDataDynDbg(const lldb::ProcessWP &process_wp)
      : StructuredDataPlugin(process_wp) {}

  static lldb::StructuredDataPluginSP CreateInstance(Process &process);
  static void DebuggerInitialize(Debugger &debugger);
};

} // namespace lldb_private

#endif
