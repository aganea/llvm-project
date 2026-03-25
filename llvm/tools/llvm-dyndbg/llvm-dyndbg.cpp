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

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
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
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compression.h"
#include "llvm/Support/Endian.h"
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

static cl::opt<std::string> PDBPath(cl::Positional, cl::desc("<pdb-file>"));

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

static cl::opt<std::string> ExtractBC(
    "extract-bc",
    cl::desc("Extract .dyndbg bitcode from a COFF .obj and write to this path"),
    cl::init(""));

static cl::opt<std::string> InputObj(
    "input-obj",
    cl::desc("Input .obj file for --extract-bc"),
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

/// Drop precompiled-header arguments from a deoptimization recompile.  PCH
/// files are tied to the optimization level they were built with; replaying
/// LF_BUILDINFO with `-O0` after an `-O2`/`-O3` build hits "OptimizationLevel
/// differs in precompiled file".  Removing PCH forces a normal parse (slower
/// but correct).  Also strips dependency-scanning flags not needed for .obj.
static void stripPchArgsForDeoptRecompile(SmallVectorImpl<std::string> &Args) {
  for (size_t I = 0; I < Args.size();) {
    StringRef A = Args[I];
    if (A == "-include-pch" && I + 1 < Args.size()) {
      Args.erase(Args.begin() + I, Args.begin() + I + 2);
      continue;
    }
    if (A.starts_with("-include-pch=")) {
      Args.erase(Args.begin() + I);
      continue;
    }
    if (A.starts_with("-pch-through-header=")) {
      Args.erase(Args.begin() + I);
      continue;
    }
    if (A == "-pch-through-header" && I + 1 < Args.size()) {
      Args.erase(Args.begin() + I, Args.begin() + I + 2);
      continue;
    }
    // Keep `-include .../cmake_pch.hxx`: without `-include-pch`, Clang parses it
    // as a normal header so the TU still sees the same preamble (slow but valid).
    // Driver-style: -Xclang -include-pch -Xclang <path>
    if (A == "-Xclang" && I + 3 < Args.size() && Args[I + 1] == "-include-pch" &&
        Args[I + 2] == "-Xclang") {
      Args.erase(Args.begin() + I, Args.begin() + I + 4);
      continue;
    }
    if (A == "--show-includes" || A == "-sys-header-deps") {
      Args.erase(Args.begin() + I);
      continue;
    }
    ++I;
  }
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

  // PCH from the optimized build does not match -O0 (see Clang's pch validate).
  stripPchArgsForDeoptRecompile(Args);

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

/// Read the .dyndbg section from a COFF object, decompress it, and return
/// the raw bitcode as a MemoryBuffer.  Returns nullptr on error (with
/// diagnostics printed to errs()).
static std::unique_ptr<MemoryBuffer>
extractBitcodeBuffer(StringRef ObjPath) {
  auto BufOrErr = MemoryBuffer::getFile(ObjPath);
  if (!BufOrErr) {
    errs() << "error: cannot open " << ObjPath << ": "
           << BufOrErr.getError().message() << "\n";
    return nullptr;
  }

  auto ObjOrErr = object::ObjectFile::createObjectFile(
      (*BufOrErr)->getMemBufferRef());
  if (!ObjOrErr) {
    errs() << "error: cannot parse object file: "
           << toString(ObjOrErr.takeError()) << "\n";
    return nullptr;
  }

  const auto &Obj = **ObjOrErr;
  for (const auto &Sec : Obj.sections()) {
    auto NameOrErr = Sec.getName();
    if (!NameOrErr) {
      consumeError(NameOrErr.takeError());
      continue;
    }
    if (*NameOrErr != ".dyndbg")
      continue;

    auto ContentsOrErr = Sec.getContents();
    if (!ContentsOrErr) {
      errs() << "error: cannot read .dyndbg section: "
             << toString(ContentsOrErr.takeError()) << "\n";
      return nullptr;
    }

    StringRef Data = *ContentsOrErr;
    if (Data.size() < 9 || Data.substr(0, 4) != "DYDB") {
      errs() << "error: .dyndbg section has invalid header\n";
      return nullptr;
    }

    uint32_t UncompressedSize =
        support::endian::read32le(Data.data() + 4);
    uint8_t IsCompressed = static_cast<uint8_t>(Data[8]);
    ArrayRef<uint8_t> Payload(
        reinterpret_cast<const uint8_t *>(Data.data() + 9),
        Data.size() - 9);

    SmallVector<uint8_t, 0> Decompressed;
    if (IsCompressed) {
      if (!compression::zstd::isAvailable()) {
        errs() << "error: Zstd decompression not available\n";
        return nullptr;
      }
      if (auto Err = compression::zstd::decompress(
              Payload, Decompressed, UncompressedSize)) {
        errs() << "error: decompression failed: " << toString(std::move(Err))
               << "\n";
        return nullptr;
      }
    } else {
      Decompressed.assign(Payload.begin(), Payload.end());
    }

    return MemoryBuffer::getMemBufferCopy(
        StringRef(reinterpret_cast<const char *>(Decompressed.data()),
                  Decompressed.size()),
        "dyndbg.bitcode");
  }

  errs() << "error: no .dyndbg section found in " << ObjPath << "\n";
  return nullptr;
}

/// Read the .dyndbg section from a COFF object, decompress it, and write
/// the resulting bitcode to \p OutputPath.
static int extractBitcodeFromObj(StringRef ObjPath, StringRef OutputPath) {
  auto Buf = extractBitcodeBuffer(ObjPath);
  if (!Buf)
    return 1;

  std::error_code EC;
  raw_fd_ostream OutFile(OutputPath, EC, sys::fs::OF_None);
  if (EC) {
    errs() << "error: cannot write " << OutputPath << ": " << EC.message()
           << "\n";
    return 1;
  }
  OutFile.write(Buf->getBufferStart(), Buf->getBufferSize());

  outs() << "Extracted " << Buf->getBufferSize() << " bytes of bitcode from "
         << ObjPath << " to " << OutputPath << "\n";
  return 0;
}

// ── Bitcode cache ──────────────────────────────────────────────────────────

/// Return the decompressed bitcode for \p ObjPath, using a file-based cache
/// keyed by \p TUHash.  On cache miss the .dyndbg section is extracted from
/// the object file, decompressed, and written into the cache directory.
/// \p CacheHit is set to indicate whether the cache was used.
static std::unique_ptr<MemoryBuffer>
getOrCacheBitcode(StringRef ObjPath, StringRef TUHash, StringRef OutDir,
                  bool &CacheHit) {
  CacheHit = false;

  SmallString<256> CacheDir;
  if (!OutDir.empty())
    CacheDir = OutDir;
  else
    sys::path::system_temp_directory(/*ErasedOnReboot=*/true, CacheDir);
  sys::path::append(CacheDir, ".dyndbg-cache");

  SmallString<256> CachePath(CacheDir);
  std::string CacheFilename;
  if (TUHash.empty())
    CacheFilename = (sys::path::stem(ObjPath) + ".bc").str();
  else
    CacheFilename = (sys::path::stem(ObjPath) + "." + TUHash + ".bc").str();
  sys::path::append(CachePath, CacheFilename);

  if (sys::fs::exists(CachePath)) {
    auto BufOrErr = MemoryBuffer::getFile(CachePath);
    if (BufOrErr && (*BufOrErr)->getBufferSize() > 0) {
      CacheHit = true;
      return std::move(*BufOrErr);
    }
  }

  auto Buf = extractBitcodeBuffer(ObjPath);
  if (!Buf)
    return nullptr;

  if (auto EC = sys::fs::create_directories(CacheDir)) {
    errs() << "warning: cannot create cache dir " << CacheDir << ": "
           << EC.message() << "\n";
  } else {
    std::error_code WEC;
    raw_fd_ostream CacheFile(CachePath, WEC, sys::fs::OF_None);
    if (!WEC)
      CacheFile.write(Buf->getBufferStart(), Buf->getBufferSize());
  }

  return Buf;
}

// ── Bitcode thinning ───────────────────────────────────────────────────────

static const GlobalObject *resolveUltimateAliasee(const GlobalAlias &GA) {
  const Constant *C = GA.getAliasee();
  SmallPtrSet<const GlobalAlias *, 8> Visited;
  for (;;) {
    C = C->stripPointerCasts();
    const auto *Inner = dyn_cast<GlobalAlias>(C);
    if (!Inner)
      return dyn_cast<GlobalObject>(C);
    if (!Visited.insert(Inner).second)
      return nullptr;
    C = Inner->getAliasee();
  }
}

/// Lazy-load the bitcode in \p BCBuf, keep only the function matching
/// \p KeepName (substring match), externalize everything else, prune
/// unreferenced declarations, and write the thinned bitcode to \p OutputBC.
/// Returns the number of pruned functions via \p PrunedFns / \p PrunedGVs,
/// or -1 on error.
static int thinBitcode(MemoryBuffer &BCBuf, StringRef KeepName,
                       StringRef TUHash, StringRef OutputBC,
                       unsigned &PrunedFns, unsigned &PrunedGVs) {
  PrunedFns = 0;
  PrunedGVs = 0;

  LLVMContext Ctx;
  auto ModOrErr = parseBitcodeFile(BCBuf.getMemBufferRef(), Ctx);
  if (!ModOrErr) {
    errs() << "error: failed to load bitcode: "
           << toString(ModOrErr.takeError()) << "\n";
    return -1;
  }
  std::unique_ptr<Module> M = std::move(*ModOrErr);

  // Find the target function by substring match.
  Function *KeepFn = nullptr;
  for (Function &F : *M) {
    if (F.isDeclaration())
      continue;
    if (F.getName().contains(KeepName)) {
      KeepFn = &F;
      break;
    }
  }
  if (!KeepFn) {
    errs() << "error: function '" << KeepName
           << "' not found in bitcode module\n";
    return -1;
  }

  // Rename the kept function to avoid clashing with the optimized version.
  KeepFn->setName((KeepFn->getName() + ".dyndbg.unopt").str());

  auto IsSpecialGlobal = [](const GlobalVariable &GV) {
    StringRef Name = GV.getName();
    return Name.starts_with("llvm.") || Name.starts_with("__") ||
           GV.getSection().starts_with(".llvm");
  };

  auto ShouldExternFn = [&](const Function &F) -> bool {
    if (&F == KeepFn || F.isDeclaration() || F.isIntrinsic())
      return false;
    return true;
  };

  auto ShouldExternGV = [&](const GlobalVariable &GV) -> bool {
    if (GV.isDeclaration())
      return false;
    if (IsSpecialGlobal(GV))
      return false;
    return true;
  };

  // Handle GlobalAliases that point to functions/globals we will externalize.
  // Aliases must point to definitions; redirect uses to the ultimate aliasee
  // then erase.
  bool AliasProgress = true;
  while (AliasProgress) {
    AliasProgress = false;
    for (GlobalAlias &GA : make_early_inc_range(M->aliases())) {
      const GlobalObject *Ultimate = resolveUltimateAliasee(GA);
      if (!Ultimate)
        continue;
      bool Drop = false;
      if (const auto *F = dyn_cast<Function>(Ultimate))
        Drop = ShouldExternFn(*F);
      else if (const auto *GV = dyn_cast<GlobalVariable>(Ultimate))
        Drop = ShouldExternGV(*GV);
      if (!Drop)
        continue;

      auto *Repl = cast<Constant>(const_cast<GlobalObject *>(Ultimate));
      if (Repl->getType() != GA.getType())
        Repl = ConstantExpr::getPointerBitCastOrAddrSpaceCast(Repl,
                                                               GA.getType());
      GA.replaceAllUsesWith(Repl);
      GA.eraseFromParent();
      AliasProgress = true;
      break;
    }
  }

  // Externalize all globals except the kept function's direct references.
  for (GlobalVariable &GV : make_early_inc_range(M->globals())) {
    if (!ShouldExternGV(GV))
      continue;

    if (GV.hasLocalLinkage())
      GV.setName((GV.getName() + ".dyndbg." + TUHash).str());

    GV.setInitializer(nullptr);
    GV.setLinkage(GlobalValue::ExternalLinkage);
    GV.setVisibility(GlobalValue::DefaultVisibility);
    GV.setDSOLocal(false);
    if (GV.hasComdat())
      GV.setComdat(nullptr);
    ++PrunedGVs;
  }

  // Externalize all functions except the kept one.
  for (Function &F : make_early_inc_range(*M)) {
    if (!ShouldExternFn(F))
      continue;

    if (F.hasLocalLinkage())
      F.setName((F.getName() + ".dyndbg." + TUHash).str());

    F.deleteBody();
    F.setLinkage(GlobalValue::ExternalLinkage);
    F.setVisibility(GlobalValue::DefaultVisibility);
    F.setDSOLocal(false);
    if (F.hasComdat())
      F.setComdat(nullptr);
    ++PrunedFns;
  }

  // Prune unreferenced declarations (fixpoint loop).
  bool Progress = true;
  while (Progress) {
    Progress = false;
    for (Function &F : make_early_inc_range(*M)) {
      if (F.isDeclaration() && F.use_empty() && &F != KeepFn) {
        F.eraseFromParent();
        Progress = true;
      }
    }
    for (GlobalVariable &GV : make_early_inc_range(M->globals())) {
      if (GV.isDeclaration() && GV.use_empty()) {
        GV.eraseFromParent();
        Progress = true;
      }
    }
    for (GlobalAlias &GA : make_early_inc_range(M->aliases())) {
      if (GA.use_empty()) {
        GA.eraseFromParent();
        Progress = true;
      }
    }
  }

  // Write the thinned module.
  std::error_code EC;
  raw_fd_ostream OutFile(OutputBC, EC, sys::fs::OF_None);
  if (EC) {
    errs() << "error: cannot write " << OutputBC << ": " << EC.message()
           << "\n";
    return -1;
  }
  WriteBitcodeToFile(*M, OutFile);

  return 0;
}

// ── Bitcode-based recompile command ────────────────────────────────────────

/// Build a minimal `clang -cc1 -x ir` command that only does codegen on the
/// already-thinned bitcode file.  The triple is extracted from the original
/// cc1 command stored in the PDB's LF_BUILDINFO.
static std::string buildBitcodeRecompileCommand(const BuildInfo &BI,
                                                StringRef ThinBCPath,
                                                StringRef OutputObj) {
  auto OrigArgs = parseCommandLine(BI.CommandLine);

  // Extract the target triple from the original cc1 args.
  std::string Triple;
  for (size_t I = 0, E = OrigArgs.size(); I + 1 < E; ++I) {
    if (OrigArgs[I] == "-triple") {
      Triple = OrigArgs[I + 1];
      break;
    }
  }

  SmallVector<std::string, 32> Args;
  Args.push_back("-cc1");
  if (!Triple.empty()) {
    Args.push_back("-triple");
    Args.push_back(Triple);
  }
  Args.push_back("-emit-obj");
  Args.push_back("-x");
  Args.push_back("ir");
  Args.push_back("-O0");
  Args.push_back("-o");
  Args.push_back(std::string(OutputObj));
  Args.push_back(std::string(ThinBCPath));

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

// ── Timer helper ───────────────────────────────────────────────────────────

using Clock = std::chrono::high_resolution_clock;

static double elapsedMs(Clock::time_point Start, Clock::time_point End) {
  return std::chrono::duration<double, std::milli>(End - Start).count();
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  ExitOnErr.setBanner("llvm-dyndbg: ");

  cl::ParseCommandLineOptions(argc, argv, "Dynamic debugging PoC tool\n");

  // --extract-bc mode: read .dyndbg section, decompress, write .bc file.
  if (!ExtractBC.empty()) {
    if (InputObj.empty()) {
      errs() << "error: --extract-bc requires --input-obj\n";
      return 1;
    }
    return extractBitcodeFromObj(InputObj, ExtractBC);
  }

  // --function implies --gen-recompile.
  if (!FunctionName.empty())
    GenRecompileCmd = true;

  if (PDBPath.empty()) {
    errs() << "error: a PDB file is required (unless using --extract-bc)\n";
    return 1;
  }

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

    if (!FunctionName.empty()) {
      // ── Bitcode-based thin deoptimization pipeline ──
      FunctionFound = true;

      SmallString<256> OutObjDir;
      if (!OutputDir.empty())
        OutObjDir = OutputDir;
      else
        OutObjDir = BI->WorkingDir;
      SmallString<256> OutputObjPath(OutObjDir);
      sys::path::append(OutputObjPath,
                        sys::path::stem(BI->SourceFile).str() + ".dyndbg.obj");

      // Resolve the actual .obj path.  When the module lives inside a .lib,
      // ObjName points to the archive; the individual .obj is at
      // WorkingDir/ModName.
      SmallString<256> ResolvedObjPath;
      if (ObjName.ends_with(".lib") || ObjName.ends_with(".a")) {
        ResolvedObjPath = BI->WorkingDir;
        sys::path::append(ResolvedObjPath, ModName);
      } else {
        ResolvedObjPath = ObjName;
      }
      outs() << "  ResolvedObj: " << ResolvedObjPath << "\n";

      // [1/4] Extract bitcode (with cache).
      auto T1 = Clock::now();
      bool CacheHit = false;
      auto BCBuf = getOrCacheBitcode(ResolvedObjPath, DynDbgHash, OutputDir,
                                     CacheHit);
      auto T2 = Clock::now();
      if (!BCBuf) {
        errs() << "  ERROR: failed to extract bitcode from " << ResolvedObjPath
               << "\n";
        outs() << "\n";
        continue;
      }
      outs() << "  [1/4] Extract bitcode: " << format("%.1f", elapsedMs(T1, T2))
             << " ms (" << (CacheHit ? "from cache" : "from .obj") << ", "
             << format("%.1f", BCBuf->getBufferSize() / 1024.0) << " KB)\n";

      // [2/4] Lazy load + thin.
      SmallString<256> ThinBCPath(OutObjDir);
      sys::path::append(ThinBCPath,
                        sys::path::stem(BI->SourceFile).str() + ".thin.bc");
      auto T3 = Clock::now();
      unsigned PrunedFns = 0, PrunedGVs = 0;
      int ThinRC = thinBitcode(*BCBuf, FunctionName, DynDbgHash,
                               ThinBCPath, PrunedFns, PrunedGVs);
      auto T4 = Clock::now();
      if (ThinRC != 0) {
        errs() << "  ERROR: thinning failed\n";
        outs() << "\n";
        continue;
      }
      uint64_t ThinSize = 0;
      sys::fs::file_size(ThinBCPath, ThinSize);
      outs() << "  [2/4] Thin module:     " << format("%.1f", elapsedMs(T3, T4))
             << " ms (pruned " << PrunedFns << " fn + " << PrunedGVs
             << " gv, thinned bc: " << format("%.1f", ThinSize / 1024.0)
             << " KB)\n";

      // [3/4] Codegen via clang -cc1 -x ir.
      std::string CodegenCmd =
          buildBitcodeRecompileCommand(*BI, ThinBCPath, OutputObjPath);
      outs() << "  CodegenCmd: " << CodegenCmd << "\n";

      if (Execute) {
        auto T5 = Clock::now();
        std::string ErrMsg;
        double CodegenMs = executeRecompile(CodegenCmd, ErrMsg);
        auto T6 = Clock::now();

        if (CodegenMs < 0) {
          errs() << "  ERROR: codegen failed: " << ErrMsg << "\n";
        } else {
          outs() << "  [3/4] Codegen:         "
                 << format("%.1f", elapsedMs(T5, T6)) << " ms\n";

          double TotalMs = elapsedMs(T1, T6);
          outs() << "  Total: " << format("%.1f", TotalMs) << " ms\n";

          uint64_t ObjSize = 0;
          if (!sys::fs::file_size(OutputObjPath, ObjSize))
            outs() << "  Output: " << OutputObjPath << " ("
                   << format("%.1f", ObjSize / 1024.0) << " KB)\n";
        }
      }
    } else if (GenRecompileCmd) {
      // ── Legacy source-based recompilation path ──
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
