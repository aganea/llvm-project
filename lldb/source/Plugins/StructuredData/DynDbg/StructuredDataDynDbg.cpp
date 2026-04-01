//===-- StructuredDataDynDbg.cpp -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "StructuredDataDynDbg.h"
#include "DynDbgDeoptimizer.h"

#include "lldb/Breakpoint/Breakpoint.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandObject.h"
#include "lldb/Interpreter/CommandObjectMultiword.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Interpreter/OptionGroupString.h"
#include "lldb/Target/Target.h"

using namespace lldb;
using namespace lldb_private;

LLDB_PLUGIN_DEFINE(StructuredDataDynDbg)

// ---------------------------------------------------------------------------
// Command: dyndbg deoptimize <function-name>
// ---------------------------------------------------------------------------
class CommandObjectDynDbgDeoptimize : public CommandObjectParsed {
public:
  CommandObjectDynDbgDeoptimize(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "dyndbg deoptimize",
            "Deoptimize a function: extract pre-opt bitcode from .dyndbg, "
            "codegen at -O0, load into debuggee, and patch function entry.",
            "dyndbg deoptimize <function-name>") {
    AddSimpleArgumentList(eArgTypeFunctionName);
  }

  ~CommandObjectDynDbgDeoptimize() override = default;

protected:
  void DoExecute(Args &args, CommandReturnObject &result) override {
    if (args.GetArgumentCount() != 1) {
      result.AppendError("Usage: dyndbg deoptimize <function-name>");
      return;
    }

    Target &target = GetTarget();
    llvm::StringRef func_name = args[0].ref();

    DynDbgDeoptimizer &deopt = DynDbgDeoptimizer::GetForTarget(target);
    if (deopt.IsDeoptimized(func_name)) {
      result.AppendWarningWithFormat("Function '%s' is already deoptimized.\n",
                                    func_name.str().c_str());
      result.SetStatus(eReturnStatusSuccessFinishResult);
      return;
    }

    Status status = deopt.Deoptimize(func_name, result.GetOutputStream());
    if (status.Fail()) {
      result.AppendError(status.AsCString());
      return;
    }

    result.SetStatus(eReturnStatusSuccessFinishResult);
  }
};

// ---------------------------------------------------------------------------
// Command: dyndbg reoptimize <function-name>
// ---------------------------------------------------------------------------
class CommandObjectDynDbgReoptimize : public CommandObjectParsed {
public:
  CommandObjectDynDbgReoptimize(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "dyndbg reoptimize",
            "Restore the optimized version of a previously deoptimized "
            "function.",
            "dyndbg reoptimize <function-name | --all>") {
    AddSimpleArgumentList(eArgTypeFunctionName, eArgRepeatOptional);
  }

  ~CommandObjectDynDbgReoptimize() override = default;

protected:
  void DoExecute(Args &args, CommandReturnObject &result) override {
    Target &target = GetTarget();
    DynDbgDeoptimizer &deopt = DynDbgDeoptimizer::GetForTarget(target);

    if (args.GetArgumentCount() == 0) {
      Status status = deopt.ReoptimizeAll(result.GetOutputStream());
      if (status.Fail()) {
        result.AppendError(status.AsCString());
        return;
      }
    } else {
      llvm::StringRef func_name = args[0].ref();
      Status status = deopt.Reoptimize(func_name, result.GetOutputStream());
      if (status.Fail()) {
        result.AppendError(status.AsCString());
        return;
      }
    }

    result.SetStatus(eReturnStatusSuccessFinishResult);
  }
};

// ---------------------------------------------------------------------------
// Command: dyndbg status
// ---------------------------------------------------------------------------
class CommandObjectDynDbgStatus : public CommandObjectParsed {
public:
  CommandObjectDynDbgStatus(CommandInterpreter &interpreter)
      : CommandObjectParsed(interpreter, "dyndbg status",
                            "Show currently deoptimized functions.",
                            "dyndbg status") {}

  ~CommandObjectDynDbgStatus() override = default;

protected:
  void DoExecute(Args &args, CommandReturnObject &result) override {
    Target &target = GetTarget();
    DynDbgDeoptimizer &deopt = DynDbgDeoptimizer::GetForTarget(target);
    deopt.PrintStatus(result.GetOutputStream());
    result.SetStatus(eReturnStatusSuccessFinishResult);
  }
};

// ---------------------------------------------------------------------------
// Command: dyndbg break <function-name>
// ---------------------------------------------------------------------------
class CommandObjectDynDbgBreak : public CommandObjectParsed {
public:
  CommandObjectDynDbgBreak(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "dyndbg break",
            "Deoptimize a function (if needed) and set a breakpoint on it.",
            "dyndbg break <function-name>") {
    AddSimpleArgumentList(eArgTypeFunctionName);
  }

  ~CommandObjectDynDbgBreak() override = default;

protected:
  void DoExecute(Args &args, CommandReturnObject &result) override {
    if (args.GetArgumentCount() != 1) {
      result.AppendError("Usage: dyndbg break <function-name>");
      return;
    }

    Target &target = GetTarget();
    llvm::StringRef func_name = args[0].ref();

    DynDbgDeoptimizer &deopt = DynDbgDeoptimizer::GetForTarget(target);
    if (!deopt.IsDeoptimized(func_name)) {
      Status status = deopt.Deoptimize(func_name, result.GetOutputStream());
      if (status.Fail()) {
        result.AppendWarningWithFormat(
            "Deoptimization failed: %s\nSetting breakpoint on optimized "
            "function.\n",
            status.AsCString());
      }
    }

    BreakpointSP bp_sp = target.CreateBreakpoint(
        /*containingModules=*/nullptr, /*containingSourceFiles=*/nullptr,
        func_name.str().c_str(),
        eFunctionNameTypeAuto, eLanguageTypeUnknown,
        /*offset=*/0, /*offset_is_insn_count=*/false,
        /*skip_prologue=*/eLazyBoolCalculate,
        /*internal=*/false, /*request_hardware=*/false);

    if (bp_sp) {
      result.GetOutputStream().Printf(
          "Breakpoint %u: %s\n", bp_sp->GetID(),
          func_name.str().c_str());
      result.SetStatus(eReturnStatusSuccessFinishResult);
    } else {
      result.AppendErrorWithFormat("Failed to set breakpoint on '%s'.\n",
                                  func_name.str().c_str());
    }
  }
};

// ---------------------------------------------------------------------------
// Multiword command: dyndbg
// ---------------------------------------------------------------------------
class CommandObjectDynDbg : public CommandObjectMultiword {
public:
  CommandObjectDynDbg(CommandInterpreter &interpreter)
      : CommandObjectMultiword(
            interpreter, "dyndbg",
            "Commands for on-demand deoptimization of C++ functions. "
            "Reads pre-optimization bitcode embedded in .obj files, "
            "extracts a single function, codegens at -O0, loads into "
            "the debuggee, and patches the optimized entry with JMP.",
            "dyndbg <subcommand> [<args>]") {
    LoadSubCommand(
        "break",
        CommandObjectSP(new CommandObjectDynDbgBreak(interpreter)));
    LoadSubCommand(
        "deoptimize",
        CommandObjectSP(new CommandObjectDynDbgDeoptimize(interpreter)));
    LoadSubCommand(
        "reoptimize",
        CommandObjectSP(new CommandObjectDynDbgReoptimize(interpreter)));
    LoadSubCommand(
        "status", CommandObjectSP(new CommandObjectDynDbgStatus(interpreter)));
  }

  ~CommandObjectDynDbg() override = default;
};

// ---------------------------------------------------------------------------
// Plugin registration
// ---------------------------------------------------------------------------

StructuredDataPluginSP
StructuredDataDynDbg::CreateInstance(Process &process) {
  return nullptr;
}

void StructuredDataDynDbg::DebuggerInitialize(Debugger &debugger) {
  auto &interpreter = debugger.GetCommandInterpreter();
  if (!interpreter.GetCommandObject("dyndbg")) {
    interpreter.AddCommand(
        "dyndbg", CommandObjectSP(new CommandObjectDynDbg(interpreter)),
        /*can_replace=*/false);
  }
}

void StructuredDataDynDbg::Initialize() {
  PluginManager::RegisterPlugin(GetStaticPluginName(),
                                "Dynamic debugging (on-demand deoptimization)",
                                &CreateInstance, &DebuggerInitialize);
}

void StructuredDataDynDbg::Terminate() {
  PluginManager::UnregisterPlugin(&CreateInstance);
}
