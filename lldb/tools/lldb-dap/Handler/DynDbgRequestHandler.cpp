//===-- DynDbgRequestHandler.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// DAP protocol handlers for on-demand deoptimization (dyndbg).
//
// Custom requests:
//   - __lldb_dyndbgDeoptimize: Deoptimize a function by name.
//   - __lldb_dyndbgReoptimize: Restore optimized version of a function.
//   - __lldb_dyndbgStatus:     List currently deoptimized functions.
//
//===----------------------------------------------------------------------===//

#include "DAP.h"
#include "RequestHandler.h"
#include "lldb/API/SBCommandInterpreter.h"
#include "lldb/API/SBCommandReturnObject.h"

using namespace lldb_dap;
using namespace lldb_dap::protocol;

// ---------------------------------------------------------------------------
// __lldb_dyndbgDeoptimize
// ---------------------------------------------------------------------------

llvm::Expected<DynDbgDeoptimizeResponseBody>
DynDbgDeoptimizeRequestHandler::Run(
    const DynDbgDeoptimizeArguments &args) const {
  lldb::SBCommandInterpreter interp = dap.debugger.GetCommandInterpreter();
  lldb::SBCommandReturnObject result;

  std::string cmd = "dyndbg deoptimize " + args.functionName;
  interp.HandleCommand(cmd.c_str(), result);

  DynDbgDeoptimizeResponseBody body;
  body.success = result.Succeeded();
  if (result.GetOutput())
    body.message = result.GetOutput();
  if (!body.success && result.GetError())
    body.message = result.GetError();

  return body;
}

// ---------------------------------------------------------------------------
// __lldb_dyndbgReoptimize
// ---------------------------------------------------------------------------

llvm::Expected<DynDbgReoptimizeResponseBody>
DynDbgReoptimizeRequestHandler::Run(
    const DynDbgReoptimizeArguments &args) const {
  lldb::SBCommandInterpreter interp = dap.debugger.GetCommandInterpreter();
  lldb::SBCommandReturnObject result;

  std::string cmd = "dyndbg reoptimize";
  if (!args.functionName.empty())
    cmd += " " + args.functionName;

  interp.HandleCommand(cmd.c_str(), result);

  DynDbgReoptimizeResponseBody body;
  body.success = result.Succeeded();
  if (result.GetOutput())
    body.message = result.GetOutput();
  if (!body.success && result.GetError())
    body.message = result.GetError();

  return body;
}

// ---------------------------------------------------------------------------
// __lldb_dyndbgStatus
// ---------------------------------------------------------------------------

llvm::Expected<DynDbgStatusResponseBody>
DynDbgStatusRequestHandler::Run(const DynDbgStatusArguments &) const {
  lldb::SBCommandInterpreter interp = dap.debugger.GetCommandInterpreter();
  lldb::SBCommandReturnObject result;

  interp.HandleCommand("dyndbg status", result);

  DynDbgStatusResponseBody body;
  if (result.GetOutput())
    body.status = result.GetOutput();

  return body;
}
