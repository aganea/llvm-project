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
// Optionally generates the -O0 recompilation command for a given module.
//
//===----------------------------------------------------------------------===//

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
#include "llvm/DebugInfo/PDB/Native/TpiStream.h"
#include "llvm/DebugInfo/PDB/PDB.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

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

static std::string buildRecompileCommand(const BuildInfo &BI,
                                         StringRef OutDir) {
  // Take the stored -cc1 command line, replace optimization flags with -O0,
  // and add the source file and output path.
  std::string Cmd = BI.CommandLine;

  // Strip existing optimization flags.
  auto ReplaceFlag = [&Cmd](StringRef Old, StringRef New) {
    size_t Pos = Cmd.find(Old.str());
    if (Pos != std::string::npos)
      Cmd.replace(Pos, Old.size(), New.str());
  };

  // Replace -O2/-O3/-O1 with -O0 in the stored cc1 line.
  bool Replaced = false;
  for (StringRef OptFlag : {"-O3", "-O2", "-O1", "-Os", "-Oz"}) {
    size_t Pos = Cmd.find(OptFlag.str());
    if (Pos != std::string::npos) {
      Cmd.replace(Pos, OptFlag.size(), "-O0");
      Replaced = true;
      break;
    }
  }
  if (!Replaced) {
    // No optimization flag found; prepend -O0.
    Cmd = "-O0 " + Cmd;
  }

  // Remove "-fdynamic-debug-prep" (possibly quoted) from the command line.
  for (StringRef Pat :
       {"\" \"-fdynamic-debug-prep\"", "\"-fdynamic-debug-prep\"",
        " -fdynamic-debug-prep", "-fdynamic-debug-prep"}) {
    size_t Pos = Cmd.find(Pat.str());
    if (Pos != std::string::npos) {
      size_t End = Pos + Pat.size();
      while (End < Cmd.size() && Cmd[End] == ' ')
        ++End;
      Cmd.erase(Pos, End - Pos);
      break;
    }
  }

  // Inject -fdynamic-debug-extern so internal-linkage symbols become extern
  // declarations referencing the promoted .dyndbg.<hash> names from the PDB.
  // Insert right after the -cc1 token.
  {
    size_t CC1Pos = Cmd.find("-cc1");
    if (CC1Pos != std::string::npos) {
      size_t InsertAt = CC1Pos + strlen("-cc1");
      // Skip past the closing quote if the token is quoted.
      if (InsertAt < Cmd.size() && Cmd[InsertAt] == '"')
        ++InsertAt;
      Cmd.insert(InsertAt, " \"-fdynamic-debug-extern\"");
    }
  }

  // Build the full source path.
  SmallString<256> SourcePath;
  if (sys::path::is_absolute(BI.SourceFile))
    SourcePath = BI.SourceFile;
  else {
    SourcePath = BI.WorkingDir;
    sys::path::append(SourcePath, BI.SourceFile);
  }

  // Build output obj path.
  SmallString<256> OutputObj;
  if (!OutDir.empty())
    OutputObj = OutDir;
  else
    OutputObj = BI.WorkingDir;
  sys::path::append(OutputObj,
                    sys::path::stem(BI.SourceFile).str() + ".dyndbg.obj");

  // Construct the full command.
  std::string FullCmd;
  raw_string_ostream OS(FullCmd);
  OS << "\"" << BI.CompilerPath << "\" " << Cmd << " -main-file-name "
     << sys::path::filename(BI.SourceFile) << " -o \"" << OutputObj << "\" \""
     << SourcePath << "\"";

  return FullCmd;
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  ExitOnErr.setBanner("llvm-dyndbg: ");

  cl::ParseCommandLineOptions(argc, argv, "Dynamic debugging PoC tool\n");

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

  outs() << "PDB: " << PDBPath << "\n";
  outs() << "Modules: " << ModCount << "\n\n";

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

    auto BI = extractBuildInfo(File, IPITypes, DebugStream.getSymbolArray());
    if (!BI)
      continue;

    outs() << "Module[" << I << "]: " << ModName << "\n";
    outs() << "  ObjFile:    " << ObjName << "\n";
    outs() << "  WorkingDir: " << BI->WorkingDir << "\n";
    outs() << "  Compiler:   " << BI->CompilerPath << "\n";
    outs() << "  SourceFile: " << BI->SourceFile << "\n";
    outs() << "  CommandLine: " << BI->CommandLine << "\n";

    if (GenRecompileCmd) {
      std::string Cmd = buildRecompileCommand(*BI, OutputDir);
      outs() << "  RecompileCmd (-O0):\n    " << Cmd << "\n";
    }

    outs() << "\n";
  }

  return 0;
}
