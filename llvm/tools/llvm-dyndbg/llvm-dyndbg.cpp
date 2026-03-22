//===- llvm-dyndbg.cpp - Dynamic debugging PoC tool -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Proof-of-concept tool for dynamic debugging on COFF/CodeView.
//
// Reads a PDB file, extracts LF_BUILDINFO per compiland (TU), and prints the
// stored compiler path, working directory, source file, and command line.
// Optionally generates and executes -O0 recompilation for a specific function.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/DebugInfo/CodeView/CVSymbolVisitor.h"
#include "llvm/DebugInfo/CodeView/LazyRandomTypeCollection.h"
#include "llvm/DebugInfo/CodeView/SymbolDeserializer.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/CodeView/TypeDeserializer.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"
#include "llvm/DebugInfo/MSF/MappedBlockStream.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleDescriptor.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleList.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/InfoStream.h"
#include "llvm/DebugInfo/PDB/Native/InputFile.h"
#include "llvm/DebugInfo/PDB/Native/ModuleDebugStream.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/PublicsStream.h"
#include "llvm/DebugInfo/PDB/Native/SymbolStream.h"
#include "llvm/DebugInfo/PDB/Native/TpiStream.h"
#include "llvm/DebugInfo/PDB/PDB.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include <chrono>

using namespace llvm;
using namespace llvm::codeview;
using namespace llvm::pdb;

static cl::opt<std::string> PDBPath(cl::Positional, cl::desc("<pdb-file>"),
                                    cl::Required);

static cl::opt<bool> GenRecompileCmd(
    "gen-recompile",
    cl::desc("Generate -O0 recompilation commands for each module"),
    cl::init(false));

static cl::opt<std::string> ModuleFilter(
    "module", cl::desc("Only show info for modules matching this substring"),
    cl::init(""));

static cl::opt<std::string> FunctionName(
    "function",
    cl::desc("Function to deoptimize (matched as substring against PDB proc "
             "symbol names). Implies --gen-recompile."),
    cl::init(""));

static cl::opt<bool> Execute(
    "execute",
    cl::desc("Actually invoke the compiler to perform the -O0 recompile"),
    cl::init(false));

static cl::opt<std::string> OutputDir(
    "output-dir",
    cl::desc("Output directory for recompiled .obj files (default: temp)"),
    cl::init(""));

static ExitOnError ExitOnErr;

struct BuildInfo {
  std::string WorkingDir;
  std::string CompilerPath;
  std::string SourceFile;
  std::string TypeServerPDB;
  std::string CommandLine;
};

static StringRef resolveStringId(LazyRandomTypeCollection &Types,
                                 TypeIndex TI) {
  if (TI.isNoneType())
    return "";
  auto OptCVT = Types.tryGetType(TI);
  if (!OptCVT)
    return "";
  StringIdRecord SIR;
  if (auto Err = TypeDeserializer::deserializeAs<StringIdRecord>(*OptCVT, SIR))
    return "";
  return SIR.String;
}

static std::optional<BuildInfo>
extractBuildInfo(PDBFile &File, LazyRandomTypeCollection &IPITypes,
                 const CVSymbolArray &Symbols) {
  for (const CVSymbol &Sym : Symbols) {
    if (Sym.kind() != SymbolKind::S_BUILDINFO)
      continue;

    BuildInfoSym BIS(SymbolRecordKind::BuildInfoSym);
    if (auto Err = SymbolDeserializer::deserializeAs<BuildInfoSym>(Sym, BIS)) {
      consumeError(std::move(Err));
      continue;
    }

    auto OptCVT = IPITypes.tryGetType(BIS.BuildId);
    if (!OptCVT || OptCVT->kind() != LF_BUILDINFO)
      continue;

    BuildInfoRecord BIR;
    if (auto Err =
            TypeDeserializer::deserializeAs<BuildInfoRecord>(*OptCVT, BIR)) {
      consumeError(std::move(Err));
      continue;
    }

    BuildInfo BI;
    if (BIR.ArgIndices.size() > BuildInfoRecord::CurrentDirectory)
      BI.WorkingDir =
          resolveStringId(IPITypes, BIR.ArgIndices[BuildInfoRecord::CurrentDirectory]).str();
    if (BIR.ArgIndices.size() > BuildInfoRecord::BuildTool)
      BI.CompilerPath =
          resolveStringId(IPITypes, BIR.ArgIndices[BuildInfoRecord::BuildTool]).str();
    if (BIR.ArgIndices.size() > BuildInfoRecord::SourceFile)
      BI.SourceFile =
          resolveStringId(IPITypes, BIR.ArgIndices[BuildInfoRecord::SourceFile]).str();
    if (BIR.ArgIndices.size() > BuildInfoRecord::TypeServerPDB)
      BI.TypeServerPDB =
          resolveStringId(IPITypes, BIR.ArgIndices[BuildInfoRecord::TypeServerPDB]).str();
    if (BIR.ArgIndices.size() > BuildInfoRecord::CommandLine)
      BI.CommandLine =
          resolveStringId(IPITypes, BIR.ArgIndices[BuildInfoRecord::CommandLine]).str();

    return BI;
  }
  return std::nullopt;
}

/// Parse a cc1 command string into an argv-style vector, handling quoted args.
static SmallVector<std::string, 64> parseCommandLine(StringRef CmdStr) {
  SmallVector<std::string, 64> Args;
  bool InQuote = false;
  std::string Current;

  for (size_t I = 0, E = CmdStr.size(); I < E; ++I) {
    char C = CmdStr[I];
    if (C == '"') {
      InQuote = !InQuote;
      continue;
    }
    if (C == ' ' && !InQuote) {
      if (!Current.empty()) {
        Args.push_back(std::move(Current));
        Current.clear();
      }
      continue;
    }
    Current += C;
  }
  if (!Current.empty())
    Args.push_back(std::move(Current));

  return Args;
}

/// Scan a module's symbol stream for S_GPROC32/S_LPROC32 records and
/// return true if any procedure name contains \p Needle.
static bool modulContainsFunction(const CVSymbolArray &Symbols,
                                  StringRef Needle) {
  for (const CVSymbol &Sym : Symbols) {
    if (Sym.kind() != SymbolKind::S_GPROC32 &&
        Sym.kind() != SymbolKind::S_LPROC32)
      continue;

    ProcSym Proc(SymbolRecordKind::GlobalProcSym);
    if (auto Err =
            SymbolDeserializer::deserializeAs<ProcSym>(Sym, Proc)) {
      consumeError(std::move(Err));
      continue;
    }

    if (Proc.Name.contains(Needle))
      return true;
  }
  return false;
}

/// Scan the PDB public symbols for any ".dyndbg.<hash>" suffixed name.
/// Returns the hash portion (e.g. "c3af5896685f9d1d") or empty if not found.
static std::string extractDynDbgHash(PDBFile &File) {
  constexpr StringRef Tag = ".dyndbg.";
  auto ExpPublics = File.getPDBPublicsStream();
  if (!ExpPublics) {
    consumeError(ExpPublics.takeError());
    return "";
  }
  auto ExpSymbols = File.getPDBSymbolStream();
  if (!ExpSymbols) {
    consumeError(ExpSymbols.takeError());
    return "";
  }

  const auto &Table = ExpPublics->getPublicsTable();
  for (uint32_t Off : Table) {
    CVSymbol Sym = ExpSymbols->readRecord(Off);
    if (Sym.kind() != SymbolKind::S_PUB32)
      continue;
    PublicSym32 Pub(SymbolRecordKind::PublicSym32);
    if (auto Err =
            SymbolDeserializer::deserializeAs<PublicSym32>(Sym, Pub)) {
      consumeError(std::move(Err));
      continue;
    }
    size_t Pos = Pub.Name.find(Tag);
    if (Pos != StringRef::npos)
      return Pub.Name.substr(Pos + Tag.size()).str();
  }
  return "";
}

static std::string buildRecompileCommand(const BuildInfo &BI,
                                         StringRef OutDir,
                                         StringRef KeepFunction,
                                         StringRef TUHash) {
  // Parse the cc1 args into a clean vector for reliable manipulation.
  auto Args = parseCommandLine(BI.CommandLine);

  // Replace optimization flags with -O0.
  bool Replaced = false;
  for (auto &Arg : Args) {
    if (Arg == "-O3" || Arg == "-O2" || Arg == "-O1" || Arg == "-Os" ||
        Arg == "-Oz") {
      Arg = "-O0";
      Replaced = true;
      break;
    }
  }
  if (!Replaced)
    Args.insert(Args.begin(), "-O0");

  // Remove -fdynamic-debug-prep.
  llvm::erase_if(Args,
                 [](const std::string &A) { return A == "-fdynamic-debug-prep"; });

  // Inject -fdynamic-debug-extern right after -cc1.
  for (size_t I = 0; I < Args.size(); ++I) {
    if (Args[I] == "-cc1") {
      Args.insert(Args.begin() + I + 1, "-fdynamic-debug-extern");
      break;
    }
  }

  // If a specific function is being kept for deoptimization, tell the
  // extern pass to preserve it via -mllvm -dynamic-debug-extern-keep=<name>.
  if (!KeepFunction.empty()) {
    for (size_t I = 0; I < Args.size(); ++I) {
      if (Args[I] == "-cc1") {
        Args.insert(Args.begin() + I + 1,
                    ("-dynamic-debug-extern-keep=" + KeepFunction).str());
        Args.insert(Args.begin() + I + 1, "-mllvm");
        break;
      }
    }
  }

  // Pass the TU hash extracted from the optimized binary's PDB so the
  // extern pass produces matching symbol names.
  if (!TUHash.empty()) {
    for (size_t I = 0; I < Args.size(); ++I) {
      if (Args[I] == "-cc1") {
        Args.insert(Args.begin() + I + 1,
                    ("-dynamic-debug-extern-hash=" + TUHash).str());
        Args.insert(Args.begin() + I + 1, "-mllvm");
        break;
      }
    }
  }

  SmallString<256> OutputObj;
  if (!OutDir.empty())
    OutputObj = OutDir;
  else
    OutputObj = BI.WorkingDir;
  sys::path::append(OutputObj,
                    sys::path::stem(BI.SourceFile).str() + ".dyndbg.obj");

  // Tell clang to resolve all relative paths against the original WorkingDir.
  Args.push_back("-working-directory");
  Args.push_back(BI.WorkingDir);

  // Add output path and main-file-name (source is already in the cc1 args).
  Args.push_back("-main-file-name");
  Args.push_back(sys::path::filename(BI.SourceFile).str());
  Args.push_back("-o");
  Args.push_back(std::string(OutputObj));

  // Reassemble the command: compiler path + quoted args.
  std::string FullCmd;
  raw_string_ostream OS(FullCmd);
  OS << BI.CompilerPath;
  for (const auto &Arg : Args) {
    OS << " ";
    bool NeedsQuote = Arg.find(' ') != std::string::npos ||
                      Arg.find('\\') != std::string::npos;
    if (NeedsQuote)
      OS << "\"" << Arg << "\"";
    else
      OS << Arg;
  }

  return FullCmd;
}

/// Execute the recompile command and return the wall-clock duration in ms.
/// Returns -1 on failure.
static double executeRecompile(StringRef FullCmd, std::string &ErrMsg) {
  auto ArgStrs = parseCommandLine(FullCmd);
  if (ArgStrs.empty()) {
    ErrMsg = "empty command";
    return -1;
  }

  SmallVector<StringRef, 64> Args;
  for (const auto &S : ArgStrs)
    Args.push_back(S);

  StringRef Program = Args[0];

  auto Start = std::chrono::high_resolution_clock::now();
  bool ExecFailed = false;
  int RC = sys::ExecuteAndWait(Program, Args, /*Env=*/std::nullopt,
                               /*Redirects=*/{}, /*SecondsToWait=*/0,
                               /*MemoryLimit=*/0, &ErrMsg, &ExecFailed);
  auto End = std::chrono::high_resolution_clock::now();

  if (ExecFailed || RC != 0) {
    if (ErrMsg.empty())
      ErrMsg = "compiler returned exit code " + std::to_string(RC);
    return -1;
  }

  std::chrono::duration<double, std::milli> Elapsed = End - Start;
  return Elapsed.count();
}

/// Given the full recompile command, extract the -o <path> argument.
static std::string extractOutputPath(StringRef FullCmd) {
  auto Args = parseCommandLine(FullCmd);
  for (size_t I = 0, E = Args.size(); I + 1 < E; ++I) {
    if (Args[I] == "-o")
      return Args[I + 1];
  }
  return "";
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  ExitOnErr.setBanner("llvm-dyndbg: ");

  cl::ParseCommandLineOptions(argc, argv, "Dynamic debugging PoC tool\n");

  // --function implies --gen-recompile.
  if (!FunctionName.empty())
    GenRecompileCmd = true;

  std::unique_ptr<IPDBSession> Session;
  ExitOnErr(loadDataForPDB(PDB_ReaderType::Native, PDBPath, Session));
  auto *NS = static_cast<NativeSession *>(Session.get());
  PDBFile &File = NS->getPDBFile();

  auto ExpDbi = File.getPDBDbiStream();
  if (!ExpDbi)
    ExitOnErr(ExpDbi.takeError());
  DbiStream &Dbi = *ExpDbi;

  auto ExpIPI = File.getPDBIpiStream();
  if (!ExpIPI)
    ExitOnErr(ExpIPI.takeError());
  LazyRandomTypeCollection &IPITypes = ExpIPI->typeCollection();

  const DbiModuleList &Modules = Dbi.modules();
  uint32_t ModCount = Modules.getModuleCount();

  // Extract the TU hash from public symbols once, before iterating modules.
  std::string DynDbgHash = extractDynDbgHash(File);

  outs() << "PDB: " << PDBPath << "\n";
  outs() << "Modules: " << ModCount << "\n";
  if (!DynDbgHash.empty())
    outs() << "TU Hash: " << DynDbgHash << "\n";
  outs() << "\n";

  bool FunctionFound = false;

  for (uint32_t I = 0; I < ModCount; ++I) {
    DbiModuleDescriptor Desc = Modules.getModuleDescriptor(I);
    StringRef ModName = Desc.getModuleName();
    StringRef ObjName = Desc.getObjFileName();

    if (!ModuleFilter.empty() && !ModName.contains(ModuleFilter) &&
        !ObjName.contains(ModuleFilter))
      continue;

    uint16_t StreamIdx = Desc.getModuleStreamIndex();
    if (StreamIdx == kInvalidStreamIndex)
      continue;

    auto StreamData = File.createIndexedStream(StreamIdx);
    if (!StreamData)
      continue;

    ModuleDebugStreamRef DebugStream(Desc, std::move(StreamData));
    if (auto Err = DebugStream.reload()) {
      consumeError(std::move(Err));
      continue;
    }

    const CVSymbolArray &SymArray = DebugStream.getSymbolArray();

    // If --function is specified, skip modules that don't contain it.
    if (!FunctionName.empty() &&
        !modulContainsFunction(SymArray, FunctionName))
      continue;

    auto BI = extractBuildInfo(File, IPITypes, SymArray);
    if (!BI)
      continue;

    // Skip modules without a usable clang -cc1 command line (e.g. CRT objs).
    if (BI->CompilerPath.empty() ||
        BI->CommandLine.find("-cc1") == std::string::npos)
      continue;

    outs() << "Module[" << I << "]: " << ModName << "\n";
    outs() << "  ObjFile:    " << ObjName << "\n";
    outs() << "  WorkingDir: " << BI->WorkingDir << "\n";
    outs() << "  Compiler:   " << BI->CompilerPath << "\n";
    outs() << "  SourceFile: " << BI->SourceFile << "\n";
    outs() << "  CommandLine: " << BI->CommandLine << "\n";

    if (GenRecompileCmd) {
      std::string Cmd =
          buildRecompileCommand(*BI, OutputDir, FunctionName, DynDbgHash);
      outs() << "  RecompileCmd (-O0):\n    " << Cmd << "\n";

      if (Execute) {
        outs() << "  Executing recompile...\n";
        outs().flush();

        std::string ErrMsg;
        double ElapsedMs = executeRecompile(Cmd, ErrMsg);

        if (ElapsedMs < 0) {
          errs() << "  ERROR: recompile failed: " << ErrMsg << "\n";
        } else {
          outs() << "  Recompile time: " << format("%.1f", ElapsedMs)
                 << " ms\n";

          std::string OutPath = extractOutputPath(Cmd);
          if (!OutPath.empty()) {
            uint64_t Size = 0;
            if (!sys::fs::file_size(OutPath, Size))
              outs() << "  Output .obj size: " << Size << " bytes ("
                     << format("%.1f", Size / 1024.0) << " KB)\n";
          }
        }
      }

      if (!FunctionName.empty())
        FunctionFound = true;
    }

    outs() << "\n";
  }

  if (!FunctionName.empty() && !FunctionFound) {
    errs() << "warning: function '" << FunctionName
           << "' not found in any module\n";
    return 1;
  }

  return 0;
}
