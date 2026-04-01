//===-- DynDbgDeoptimizer.cpp ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DynDbgDeoptimizer.h"

#include "lldb/Core/Address.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleList.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/Symbol.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/SymbolFile.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Stream.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/BinaryFormat/COFF.h"
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
#include "llvm/Object/COFF.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Compression.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"

#include <chrono>
#include <string>

using namespace lldb;
using namespace lldb_private;

using Clock = std::chrono::high_resolution_clock;

static double elapsedMs(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

/// Native LLVM PDB parsing needs a filesystem path. \a Module::GetSymbolFileFileSpec()
/// is often empty on Windows because SymbolVendorPECOFF locates the PDB without
/// updating that field; the opened PDB is still reachable via \a GetSymbolFile().
static FileSpec GetNativePDBSpec(const ModuleSP &module_sp) {
  if (!module_sp)
    return {};

  module_sp->GetSymbolFile(/*can_create=*/true);

  const FileSpec from_module = module_sp->GetSymbolFileFileSpec();
  if (from_module && FileSystem::Instance().Exists(from_module))
    return from_module;

  if (SymbolFile *sf = module_sp->GetSymbolFile(/*can_create=*/false)) {
    if (ObjectFile *of = sf->GetObjectFile()) {
      const FileSpec candidate = of->GetFileSpec();
      if (candidate && FileSystem::Instance().Exists(candidate))
        return candidate;
    }
  }

  // Cannot call ObjectFilePECOFF::GetPDBPath() here: StructuredData plugins may
  // not depend on ObjectFile plugins (LLDB layering). Rely on the sidecar PDB
  // opened by the symbol vendor (above) or the heuristic below.

  llvm::SmallString<256> guess_path(module_sp->GetFileSpec().GetPath());
  llvm::sys::path::replace_extension(guess_path, "pdb");
  FileSpec guessed{llvm::StringRef(guess_path)};
  if (guessed && FileSystem::Instance().Exists(guessed))
    return guessed;

  return {};
}

// Static member definitions.
std::mutex DynDbgDeoptimizer::s_instances_mutex;
llvm::DenseMap<Target *, std::unique_ptr<DynDbgDeoptimizer>>
    DynDbgDeoptimizer::s_instances;

DynDbgDeoptimizer &DynDbgDeoptimizer::GetForTarget(Target &target) {
  std::lock_guard<std::mutex> lock(s_instances_mutex);
  auto &ptr = s_instances[&target];
  if (!ptr)
    ptr = std::unique_ptr<DynDbgDeoptimizer>(new DynDbgDeoptimizer(target));
  return *ptr;
}

// ── PDB helpers ─────────────────────────────────────────────────────────────

static llvm::StringRef
resolveStringId(llvm::codeview::LazyRandomTypeCollection &types,
                llvm::codeview::TypeIndex ti) {
  if (ti.isNoneType())
    return "";
  auto opt_cvt = types.tryGetType(ti);
  if (!opt_cvt)
    return "";
  llvm::codeview::StringIdRecord sir;
  if (auto err =
          llvm::codeview::TypeDeserializer::deserializeAs<
              llvm::codeview::StringIdRecord>(*opt_cvt, sir))
    return "";
  return sir.String;
}

static std::optional<DynDbgBuildInfo>
extractBuildInfo(llvm::pdb::PDBFile &file,
                 llvm::codeview::LazyRandomTypeCollection &ipi_types,
                 const llvm::codeview::CVSymbolArray &symbols) {
  using namespace llvm::codeview;

  for (const CVSymbol &sym : symbols) {
    if (sym.kind() != SymbolKind::S_BUILDINFO)
      continue;

    BuildInfoSym bis(SymbolRecordKind::BuildInfoSym);
    if (auto err = SymbolDeserializer::deserializeAs<BuildInfoSym>(sym, bis)) {
      llvm::consumeError(std::move(err));
      continue;
    }

    auto opt_cvt = ipi_types.tryGetType(bis.BuildId);
    if (!opt_cvt || opt_cvt->kind() != LF_BUILDINFO)
      continue;

    BuildInfoRecord bir;
    if (auto err =
            TypeDeserializer::deserializeAs<BuildInfoRecord>(*opt_cvt, bir)) {
      llvm::consumeError(std::move(err));
      continue;
    }

    DynDbgBuildInfo bi;
    if (bir.ArgIndices.size() > BuildInfoRecord::CurrentDirectory)
      bi.WorkingDir =
          resolveStringId(ipi_types,
                          bir.ArgIndices[BuildInfoRecord::CurrentDirectory])
              .str();
    if (bir.ArgIndices.size() > BuildInfoRecord::BuildTool)
      bi.CompilerPath =
          resolveStringId(ipi_types,
                          bir.ArgIndices[BuildInfoRecord::BuildTool])
              .str();
    if (bir.ArgIndices.size() > BuildInfoRecord::SourceFile)
      bi.SourceFile =
          resolveStringId(ipi_types,
                          bir.ArgIndices[BuildInfoRecord::SourceFile])
              .str();
    if (bir.ArgIndices.size() > BuildInfoRecord::TypeServerPDB)
      bi.TypeServerPDB =
          resolveStringId(ipi_types,
                          bir.ArgIndices[BuildInfoRecord::TypeServerPDB])
              .str();
    if (bir.ArgIndices.size() > BuildInfoRecord::CommandLine)
      bi.CommandLine =
          resolveStringId(ipi_types,
                          bir.ArgIndices[BuildInfoRecord::CommandLine])
              .str();

    return bi;
  }
  return std::nullopt;
}

static bool moduleContainsFunction(const llvm::codeview::CVSymbolArray &symbols,
                                   llvm::StringRef needle) {
  using namespace llvm::codeview;
  for (const CVSymbol &sym : symbols) {
    if (sym.kind() != SymbolKind::S_GPROC32 &&
        sym.kind() != SymbolKind::S_LPROC32)
      continue;

    ProcSym proc(SymbolRecordKind::GlobalProcSym);
    if (auto err = SymbolDeserializer::deserializeAs<ProcSym>(sym, proc)) {
      llvm::consumeError(std::move(err));
      continue;
    }
    if (proc.Name.contains(needle))
      return true;
  }
  return false;
}

static llvm::SmallVector<std::string, 64>
parseCommandLine(llvm::StringRef cmd_str) {
  llvm::SmallVector<std::string, 64> args;
  bool in_quote = false;
  std::string current;

  for (size_t i = 0, e = cmd_str.size(); i < e; ++i) {
    char c = cmd_str[i];
    if (c == '"') {
      in_quote = !in_quote;
      continue;
    }
    if (c == ' ' && !in_quote) {
      if (!current.empty()) {
        args.push_back(std::move(current));
        current.clear();
      }
      continue;
    }
    current += c;
  }
  if (!current.empty())
    args.push_back(std::move(current));

  return args;
}

// ── Bitcode extraction ──────────────────────────────────────────────────────

std::unique_ptr<llvm::MemoryBuffer>
DynDbgDeoptimizer::ExtractBitcodeFromObj(llvm::StringRef obj_path,
                                         std::string &error_msg) {
  auto buf_or_err = llvm::MemoryBuffer::getFile(obj_path);
  if (!buf_or_err) {
    error_msg = "cannot open " + obj_path.str() + ": " +
                buf_or_err.getError().message();
    return nullptr;
  }

  auto obj_or_err =
      llvm::object::ObjectFile::createObjectFile((*buf_or_err)->getMemBufferRef());
  if (!obj_or_err) {
    error_msg =
        "cannot parse object file: " + llvm::toString(obj_or_err.takeError());
    return nullptr;
  }

  const auto &obj = **obj_or_err;
  for (const auto &sec : obj.sections()) {
    auto name_or_err = sec.getName();
    if (!name_or_err) {
      llvm::consumeError(name_or_err.takeError());
      continue;
    }
    if (*name_or_err != ".dyndbg")
      continue;

    auto contents_or_err = sec.getContents();
    if (!contents_or_err) {
      error_msg =
          "cannot read .dyndbg section: " +
          llvm::toString(contents_or_err.takeError());
      return nullptr;
    }

    llvm::StringRef data = *contents_or_err;
    if (data.size() < 9 || data.substr(0, 4) != "DYDB") {
      error_msg = ".dyndbg section has invalid header";
      return nullptr;
    }

    uint32_t uncompressed_size =
        llvm::support::endian::read32le(data.data() + 4);
    uint8_t is_compressed = static_cast<uint8_t>(data[8]);
    llvm::ArrayRef<uint8_t> payload(
        reinterpret_cast<const uint8_t *>(data.data() + 9), data.size() - 9);

    llvm::SmallVector<uint8_t, 0> decompressed;
    if (is_compressed) {
      if (!llvm::compression::zstd::isAvailable()) {
        error_msg = "Zstd decompression not available";
        return nullptr;
      }
      if (auto err = llvm::compression::zstd::decompress(
              payload, decompressed, uncompressed_size)) {
        error_msg = "decompression failed: " + llvm::toString(std::move(err));
        return nullptr;
      }
    } else {
      decompressed.assign(payload.begin(), payload.end());
    }

    return llvm::MemoryBuffer::getMemBufferCopy(
        llvm::StringRef(reinterpret_cast<const char *>(decompressed.data()),
                        decompressed.size()),
        "dyndbg.bitcode");
  }

  error_msg = "no .dyndbg section found in " + obj_path.str();
  return nullptr;
}

// ── Bitcode thinning ────────────────────────────────────────────────────────

static const llvm::GlobalObject *
resolveUltimateAliasee(const llvm::GlobalAlias &ga) {
  const llvm::Constant *c = ga.getAliasee();
  llvm::SmallPtrSet<const llvm::GlobalAlias *, 8> visited;
  for (;;) {
    c = c->stripPointerCasts();
    const auto *inner = llvm::dyn_cast<llvm::GlobalAlias>(c);
    if (!inner)
      return llvm::dyn_cast<llvm::GlobalObject>(c);
    if (!visited.insert(inner).second)
      return nullptr;
    c = inner->getAliasee();
  }
}

Status DynDbgDeoptimizer::ThinBitcode(llvm::MemoryBuffer &bc_buf,
                                      llvm::StringRef keep_name,
                                      llvm::StringRef tu_hash,
                                      llvm::StringRef output_path,
                                      Stream &stream) {
  llvm::LLVMContext ctx;
  auto mod_or_err = llvm::getLazyBitcodeModule(bc_buf.getMemBufferRef(), ctx);
  if (!mod_or_err)
    return Status("failed to load bitcode: " +
                  llvm::toString(mod_or_err.takeError()));

  std::unique_ptr<llvm::Module> M = std::move(*mod_or_err);

  llvm::Function *keep_fn = nullptr;
  for (llvm::Function &F : *M) {
    if (F.isDeclaration() && !F.isMaterializable())
      continue;
    if (F.getName().contains(keep_name)) {
      keep_fn = &F;
      break;
    }
  }
  if (!keep_fn)
    return Status("function '" + keep_name.str() +
                  "' not found in bitcode module");

  if (auto err = keep_fn->materialize())
    return Status("failed to materialize " + keep_fn->getName().str() + ": " +
                  llvm::toString(std::move(err)));

  keep_fn->setName((keep_fn->getName() + ".dyndbg.unopt").str());

  auto is_special_gv = [](const llvm::GlobalVariable &GV) {
    llvm::StringRef name = GV.getName();
    return name.starts_with("llvm.") || name.starts_with("__") ||
           GV.getSection().starts_with(".llvm");
  };

  auto should_extern_fn = [&](const llvm::Function &F) -> bool {
    if (&F == keep_fn || F.isIntrinsic())
      return false;
    if (F.isDeclaration() && !F.isMaterializable())
      return false;
    return true;
  };

  auto should_extern_gv = [&](const llvm::GlobalVariable &GV) -> bool {
    if (GV.isDeclaration())
      return false;
    if (is_special_gv(GV))
      return false;
    return true;
  };

  // Handle GlobalAliases pointing to functions/globals we will externalize.
  bool alias_progress = true;
  while (alias_progress) {
    alias_progress = false;
    for (llvm::GlobalAlias &GA : llvm::make_early_inc_range(M->aliases())) {
      const llvm::GlobalObject *ultimate = resolveUltimateAliasee(GA);
      if (!ultimate)
        continue;
      bool drop = false;
      if (const auto *F = llvm::dyn_cast<llvm::Function>(ultimate))
        drop = should_extern_fn(*F);
      else if (const auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(ultimate))
        drop = should_extern_gv(*GV);
      if (!drop)
        continue;

      auto *repl = llvm::cast<llvm::Constant>(
          const_cast<llvm::GlobalObject *>(ultimate));
      if (repl->getType() != GA.getType())
        repl = llvm::ConstantExpr::getPointerBitCastOrAddrSpaceCast(
            repl, GA.getType());
      GA.replaceAllUsesWith(repl);
      GA.eraseFromParent();
      alias_progress = true;
      break;
    }
  }

  // Externalize globals.
  for (llvm::GlobalVariable &GV :
       llvm::make_early_inc_range(M->globals())) {
    if (!should_extern_gv(GV))
      continue;
    if (GV.hasLocalLinkage())
      GV.setName((GV.getName() + ".dyndbg." + tu_hash).str());
    GV.setInitializer(nullptr);
    GV.setLinkage(llvm::GlobalValue::ExternalLinkage);
    GV.setVisibility(llvm::GlobalValue::DefaultVisibility);
    GV.setDSOLocal(false);
    if (GV.hasComdat())
      GV.setComdat(nullptr);
  }

  // Externalize functions.
  for (llvm::Function &F : llvm::make_early_inc_range(*M)) {
    if (!should_extern_fn(F))
      continue;
    if (F.hasLocalLinkage())
      F.setName((F.getName() + ".dyndbg." + tu_hash).str());
    F.deleteBody();
    F.setLinkage(llvm::GlobalValue::ExternalLinkage);
    F.setVisibility(llvm::GlobalValue::DefaultVisibility);
    F.setDSOLocal(false);
    if (F.hasComdat())
      F.setComdat(nullptr);
  }

  // Prune unreferenced declarations.
  bool progress = true;
  while (progress) {
    progress = false;
    for (llvm::Function &F : llvm::make_early_inc_range(*M)) {
      if (F.isDeclaration() && F.materialized_use_empty() && &F != keep_fn) {
        F.eraseFromParent();
        progress = true;
      }
    }
    for (llvm::GlobalVariable &GV : llvm::make_early_inc_range(M->globals())) {
      if (GV.isDeclaration() && GV.materialized_use_empty()) {
        GV.eraseFromParent();
        progress = true;
      }
    }
    for (llvm::GlobalAlias &GA : llvm::make_early_inc_range(M->aliases())) {
      if (GA.materialized_use_empty()) {
        GA.eraseFromParent();
        progress = true;
      }
    }
  }

  if (auto err = M->materializeAll())
    return Status("materializeAll failed: " + llvm::toString(std::move(err)));

  std::error_code ec;
  llvm::raw_fd_ostream out_file(output_path, ec, llvm::sys::fs::OF_None);
  if (ec)
    return Status("cannot write " + output_path.str() + ": " + ec.message());
  llvm::WriteBitcodeToFile(*M, out_file);

  return Status();
}

// ── Codegen ─────────────────────────────────────────────────────────────────

/// Copy cc1 flags from LF_BUILDINFO into \p out, dropping pieces that only apply
/// to parsing C/C++ or to the original optimization pipeline. Keeps target,
/// codegen, and ABI-related options (e.g. \c -target-cpu, \c -mllvm, unwind,
/// PIC, \c --dependent-lib) so the IR→obj step matches the original link.
static void appendFilteredCc1ArgsForIRCodegen(
    llvm::SmallVectorImpl<std::string> &out,
    const llvm::SmallVectorImpl<std::string> &orig) {
  auto drop_pair = [&](size_t &i) {
    if (i + 1 < orig.size())
      ++i;
  };

  for (size_t i = 0; i < orig.size(); ++i) {
    llvm::StringRef a = orig[i];

    if (a == "-cc1")
      continue;

    // Diagnostics / driver noise.
    if (a == "-fcolor-diagnostics" || a == "-disable-free" ||
        a == "-clear-ast-before-backend")
      continue;
    if (a == "-fdiagnostics-format" || a == "-ferror-limit") {
      drop_pair(i);
      continue;
    }
    if (a == "-fdiagnostics-hotness-threshold" ||
        a == "-fdiagnostics-misexpect-tolerance") {
      drop_pair(i);
      continue;
    }

    // Frontend: includes, defines, source language, original output.
    if (a == "-resource-dir" || a == "-isystem" || a == "-iquote" ||
        a == "-iframework" || a == "-I" || a == "-D" || a == "-U" ||
        a == "-include" || a == "-imacros" || a == "-MT" || a == "-MF" ||
        a == "-main-file-name" || a == "-dependency-file") {
      drop_pair(i);
      continue;
    }
    if (a.starts_with("-I") && a.size() > 2)
      continue;
    if (a.starts_with("-D") && a.size() > 2)
      continue;
    if (a.starts_with("-U") && a.size() > 2)
      continue;
    if (a.starts_with("-std="))
      continue;
    if (a == "-x") {
      drop_pair(i);
      continue;
    }
    if (a == "-o") {
      drop_pair(i);
      continue;
    }
    if (a == "-emit-obj" || a == "-emit-llvm" || a == "-S" || a == "-E")
      continue;

    // Working dirs from the original compile; not meaningful for thin IR.
    if (a.starts_with("-fdebug-compilation-dir=") ||
        a.starts_with("-fcoverage-compilation-dir="))
      continue;

    // Force deopt semantics: strip original optimization level and selected
    // pipeline toggles; RunCodegen adds -O0 explicitly.
    if (a.starts_with("-O") && a != "-Objective-C" && a != "-Objective-C++")
      continue;
    if (a == "-vectorize-loops" || a == "-vectorize-slp" ||
        a == "-finline-functions" || a == "-fno-loop-interchange")
      continue;

    // Mostly a frontend→IR concern for this compilation kind.
    if (a.starts_with("-debug-info-kind="))
      continue;

    // Two-token flags we keep verbatim (target + codegen).
    if (a == "-triple" || a == "-target-cpu" || a == "-tune-cpu" ||
        a == "-target-feature" || a == "-mllvm" || a == "-pic-level" ||
        a == "-stack-protector") {
      out.push_back(std::string(a));
      if (i + 1 < orig.size())
        out.push_back(orig[++i]);
      continue;
    }

    out.push_back(std::string(a));
  }

  while (!out.empty()) {
    llvm::StringRef last = out.back();
    if (last.ends_with(".cpp") || last.ends_with(".cxx") ||
        last.ends_with(".cc") || last.ends_with(".c") ||
        last.ends_with(".mm") || last.ends_with(".m"))
      out.pop_back();
    else
      break;
  }
}

Status DynDbgDeoptimizer::RunCodegen(const DynDbgBuildInfo &build_info,
                                     llvm::StringRef thin_bc_path,
                                     llvm::StringRef output_obj_path,
                                     Stream &stream) {
  auto orig_args = parseCommandLine(build_info.CommandLine);

  llvm::SmallVector<std::string, 64> args;
  args.push_back(build_info.CompilerPath);
  args.push_back("-cc1");
  appendFilteredCc1ArgsForIRCodegen(args, orig_args);

  bool has_triple = false;
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == "-triple") {
      has_triple = true;
      break;
    }
  }
  if (!has_triple) {
    for (size_t i = 0, e = orig_args.size(); i + 1 < e; ++i) {
      if (orig_args[i] == "-triple") {
        args.insert(args.begin() + 2, {orig_args[i], orig_args[i + 1]});
        break;
      }
    }
  }

  args.push_back("-emit-obj");
  args.push_back("-x");
  args.push_back("ir");
  args.push_back("-O0");
  args.push_back("-gcodeview");
  args.push_back("-o");
  args.push_back(output_obj_path.str());
  args.push_back(thin_bc_path.str());

  llvm::SmallVector<llvm::StringRef, 128> argv;
  for (const auto &a : args)
    argv.push_back(a);

  stream.Printf("  Codegen: %s -cc1 <filtered LF_BUILDINFO flags> -x ir -O0 "
                "-emit-obj ...\n",
                build_info.CompilerPath.c_str());

  std::string err_msg;
  bool exec_failed = false;
  auto start = Clock::now();
  int rc = llvm::sys::ExecuteAndWait(argv[0], argv, /*Env=*/std::nullopt,
                                     /*Redirects=*/{}, /*SecondsToWait=*/0,
                                     /*MemoryLimit=*/0, &err_msg, &exec_failed);
  auto end = Clock::now();

  if (exec_failed || rc != 0) {
    if (err_msg.empty())
      err_msg = "compiler returned exit code " + std::to_string(rc);
    return Status("codegen failed: " + err_msg);
  }

  stream.Printf("  Codegen completed in %.1f ms\n", elapsedMs(start, end));
  return Status();
}

// ── COFF loader ─────────────────────────────────────────────────────────────

lldb::addr_t
DynDbgDeoptimizer::ResolveSymbolAddress(llvm::StringRef name) {
  ConstString cs_name(name);

  // Search all modules in the target for this symbol.
  SymbolContextList sc_list;
  m_target.GetImages().FindSymbolsWithNameAndType(cs_name, eSymbolTypeAny,
                                                  sc_list);
  if (sc_list.GetSize() > 0) {
    SymbolContext sc;
    sc_list.GetContextAtIndex(0, sc);
    if (sc.symbol) {
      addr_t addr = sc.symbol->GetLoadAddress(&m_target);
      if (addr != LLDB_INVALID_ADDRESS)
        return addr;
    }
  }

  // Also try as a function name.
  SymbolContextList fn_list;
  ModuleFunctionSearchOptions opts;
  opts.include_inlines = false;
  opts.include_symbols = true;
  m_target.GetImages().FindFunctions(cs_name, lldb::eFunctionNameTypeFull, opts,
                                     fn_list);
  if (fn_list.GetSize() > 0) {
    SymbolContext sc;
    fn_list.GetContextAtIndex(0, sc);
    if (sc.symbol) {
      addr_t addr = sc.symbol->GetLoadAddress(&m_target);
      if (addr != LLDB_INVALID_ADDRESS)
        return addr;
    }
    if (sc.function) {
      addr_t addr = sc.function->GetAddress().GetLoadAddress(&m_target);
      if (addr != LLDB_INVALID_ADDRESS)
        return addr;
    }
  }

  return LLDB_INVALID_ADDRESS;
}

Status DynDbgDeoptimizer::LoadObjIntoDebuggee(
    llvm::StringRef obj_path, llvm::StringRef unopt_function_name,
    lldb::addr_t &unopt_addr, DynDbgPatchInfo &patch_info, Stream &stream) {
  auto buf_or_err = llvm::MemoryBuffer::getFile(obj_path);
  if (!buf_or_err)
    return Status("cannot open " + obj_path.str());

  auto obj_or_err = llvm::object::ObjectFile::createObjectFile(
      (*buf_or_err)->getMemBufferRef());
  if (!obj_or_err)
    return Status("cannot parse .obj: " +
                  llvm::toString(obj_or_err.takeError()));

  auto *coff = llvm::dyn_cast<llvm::object::COFFObjectFile>(obj_or_err->get());
  if (!coff)
    return Status("not a COFF object file");

  ProcessSP process_sp = m_target.GetProcessSP();
  if (!process_sp)
    return Status("no active process");

  // Map: COFF section index -> { debuggee base address, section data, size }.
  struct SectionInfo {
    addr_t debuggee_addr = 0;
    const uint8_t *data = nullptr;
    size_t size = 0;
    size_t allocated_size = 0; // includes REL32 trampoline pool at section tail
    uint32_t characteristics = 0;
  };
  llvm::DenseMap<unsigned, SectionInfo> section_map;

  auto amd64Rel32Addend = [](uint64_t rt) -> int {
    if (rt == llvm::COFF::IMAGE_REL_AMD64_REL32)
      return 0;
    if (rt == llvm::COFF::IMAGE_REL_AMD64_REL32_1)
      return 1;
    if (rt == llvm::COFF::IMAGE_REL_AMD64_REL32_2)
      return 2;
    if (rt == llvm::COFF::IMAGE_REL_AMD64_REL32_3)
      return 3;
    if (rt == llvm::COFF::IMAGE_REL_AMD64_REL32_4)
      return 4;
    if (rt == llvm::COFF::IMAGE_REL_AMD64_REL32_5)
      return 5;
    return -1;
  };

  auto isAmd64Rel32Family = [&](uint64_t rt) -> bool {
    return amd64Rel32Addend(rt) >= 0;
  };

  // mov r11, imm64 (10 bytes) + jmp r11 (3 bytes) + int3 pad = 16 bytes per stub.
  constexpr size_t kRel32TrampSize = 16;

  // Pass 1: for each loaded section, reserve one trampoline slot per distinct
  // symbol that may need a PC-relative fixup from this section when the real
  // target is too far (EXE vs JIT) or in another allocated section.
  llvm::DenseMap<unsigned, llvm::StringMap<unsigned>> rel32_tramp_sym_index;
  unsigned sec_idx_scan = 0;
  for (const auto &sec : coff->sections()) {
    ++sec_idx_scan;

    auto name_or_err = sec.getName();
    if (!name_or_err) {
      llvm::consumeError(name_or_err.takeError());
      continue;
    }

    auto contents_or_err = sec.getContents();
    if (!contents_or_err) {
      llvm::consumeError(contents_or_err.takeError());
      continue;
    }

    llvm::StringRef sec_data = *contents_or_err;
    if (sec_data.empty())
      continue;

    const llvm::object::coff_section *coff_sec = coff->getCOFFSection(sec);
    uint32_t chars = coff_sec->Characteristics;

    if (chars & llvm::COFF::IMAGE_SCN_LNK_REMOVE)
      continue;
    if (name_or_err->starts_with(".debug") ||
        name_or_err->starts_with(".llvm"))
      continue;

    for (const auto &reloc : sec.relocations()) {
      if (!isAmd64Rel32Family(reloc.getType()))
        continue;

      llvm::object::symbol_iterator sym_it = reloc.getSymbol();
      auto sym_name_or_err = sym_it->getName();
      if (!sym_name_or_err) {
        llvm::consumeError(sym_name_or_err.takeError());
        continue;
      }

      llvm::object::COFFSymbolRef rs = coff->getCOFFSymbol(*sym_it);
      int32_t def_sec = rs.getSectionNumber();
      int addend_i = amd64Rel32Addend(reloc.getType());
      if (addend_i < 0)
        continue;
      uint32_t addend = static_cast<uint32_t>(addend_i);

      bool need_slot = false;
      if (def_sec == llvm::COFF::IMAGE_SYM_UNDEFINED)
        need_slot = true;
      else if (def_sec == static_cast<int32_t>(sec_idx_scan)) {
        int64_t delta = (int64_t)rs.getValue() -
                        (int64_t)(reloc.getOffset() + 4 + addend);
        if (delta > INT32_MAX || delta < INT32_MIN)
          need_slot = true;
      } else
        need_slot = true;

      if (!need_slot)
        continue;

      llvm::StringMap<unsigned> &idx_map = rel32_tramp_sym_index[sec_idx_scan];
      llvm::StringRef sname = *sym_name_or_err;
      idx_map.try_emplace(sname, static_cast<unsigned>(idx_map.size()));
    }
  }

  // Allocate and copy each section into the debuggee (with trampoline tail).
  unsigned sec_idx = 0;
  for (const auto &sec : coff->sections()) {
    ++sec_idx;

    auto name_or_err = sec.getName();
    if (!name_or_err) {
      llvm::consumeError(name_or_err.takeError());
      continue;
    }

    auto contents_or_err = sec.getContents();
    if (!contents_or_err) {
      llvm::consumeError(contents_or_err.takeError());
      continue;
    }

    llvm::StringRef sec_data = *contents_or_err;
    if (sec_data.empty())
      continue;

    const llvm::object::coff_section *coff_sec = coff->getCOFFSection(sec);
    uint32_t chars = coff_sec->Characteristics;

    // Skip debug/metadata sections; we only load code and data.
    if (chars & llvm::COFF::IMAGE_SCN_LNK_REMOVE)
      continue;
    if (name_or_err->starts_with(".debug") ||
        name_or_err->starts_with(".llvm"))
      continue;

    uint32_t permissions = 0;
    if (chars & llvm::COFF::IMAGE_SCN_MEM_READ)
      permissions |= ePermissionsReadable;
    if (chars & llvm::COFF::IMAGE_SCN_MEM_WRITE)
      permissions |= ePermissionsWritable;
    if (chars & llvm::COFF::IMAGE_SCN_MEM_EXECUTE)
      permissions |= ePermissionsExecutable;
    if (permissions == 0)
      permissions = ePermissionsReadable;
    if (chars & llvm::COFF::IMAGE_SCN_CNT_CODE)
      permissions |= ePermissionsExecutable | ePermissionsReadable;

    size_t tramp_slots =
        rel32_tramp_sym_index.count(sec_idx)
            ? rel32_tramp_sym_index[sec_idx].size()
            : 0;
    size_t alloc_sz = sec_data.size() + tramp_slots * kRel32TrampSize;

    Status alloc_error;
    addr_t alloc_addr =
        process_sp->AllocateMemory(alloc_sz, permissions, alloc_error);
    if (alloc_error.Fail() || alloc_addr == LLDB_INVALID_ADDRESS)
      return Status("AllocateMemory failed for section " +
                    name_or_err->str() + ": " + alloc_error.AsCString());

    Status write_error;
    size_t written = process_sp->WriteMemory(
        alloc_addr, sec_data.data(), sec_data.size(), write_error);
    if (write_error.Fail() || written != sec_data.size())
      return Status("WriteMemory failed for section " + name_or_err->str());

    if (tramp_slots != 0) {
      std::vector<uint8_t> pad(tramp_slots * kRel32TrampSize, 0xCC);
      addr_t pad_addr = alloc_addr + sec_data.size();
      size_t pw =
          process_sp->WriteMemory(pad_addr, pad.data(), pad.size(), write_error);
      if (write_error.Fail() || pw != pad.size())
        return Status("WriteMemory failed for trampoline pool in section " +
                      name_or_err->str());
    }

    SectionInfo info;
    info.debuggee_addr = alloc_addr;
    info.data = reinterpret_cast<const uint8_t *>(sec_data.data());
    info.size = sec_data.size();
    info.allocated_size = alloc_sz;
    info.characteristics = chars;
    section_map[sec_idx] = info;

    stream.Printf("  Loaded section %s: %zu bytes at 0x%llx\n",
                  name_or_err->str().c_str(), alloc_sz,
                  (unsigned long long)alloc_addr);

    if (chars & llvm::COFF::IMAGE_SCN_CNT_CODE) {
      patch_info.AllocatedTextAddr = alloc_addr;
      patch_info.AllocatedTextSize = alloc_sz;
    } else {
      patch_info.AllocatedDataAddr = alloc_addr;
      patch_info.AllocatedDataSize = alloc_sz;
    }
  }

  // Build symbol table: map symbol name -> address in debuggee.
  llvm::StringMap<addr_t> symbol_addrs;
  for (const auto &sym : coff->symbols()) {
    auto name_or_err = sym.getName();
    if (!name_or_err) {
      llvm::consumeError(name_or_err.takeError());
      continue;
    }

    llvm::StringRef sym_name = *name_or_err;
    llvm::object::COFFSymbolRef coff_sym = coff->getCOFFSymbol(sym);
    int32_t sec_num = coff_sym.getSectionNumber();

    if (sec_num > 0 && section_map.count(sec_num)) {
      // Defined symbol: base address of its section + value (offset).
      symbol_addrs[sym_name] = section_map[sec_num].debuggee_addr +
                               static_cast<addr_t>(coff_sym.getValue());
    } else if (sec_num == llvm::COFF::IMAGE_SYM_UNDEFINED) {
      // External (undefined) symbol: resolve against the target.
      addr_t resolved = ResolveSymbolAddress(sym_name);
      if (resolved != LLDB_INVALID_ADDRESS)
        symbol_addrs[sym_name] = resolved;
      else
        stream.Printf("  WARNING: unresolved external symbol '%s'\n",
                      sym_name.str().c_str());
    }
  }

  // Emit in-section trampolines for REL32 overflow targets (near patch sites,
  // absolute jump to the real symbol — e.g. JIT .text calling the EXE).
  llvm::DenseMap<unsigned, llvm::StringMap<addr_t>> rel32_tramp_addr;
  for (const auto &tramp_sec : rel32_tramp_sym_index) {
    unsigned sidx = tramp_sec.first;
    if (!section_map.count(sidx))
      continue;
    const SectionInfo &si = section_map[sidx];
    const llvm::StringMap<unsigned> &idx_map = tramp_sec.second;

    llvm::SmallVector<llvm::StringRef, 8> ordered_names(idx_map.size());
    for (const auto &kv : idx_map) {
      unsigned idx = kv.getValue();
      if (idx < ordered_names.size())
        ordered_names[idx] = kv.getKey();
    }

    addr_t tramp_base = si.debuggee_addr + si.size;
    for (unsigned ti = 0; ti < ordered_names.size(); ++ti) {
      llvm::StringRef sym = ordered_names[ti];
      if (sym.empty())
        continue;

      auto sit = symbol_addrs.find(sym);
      if (sit == symbol_addrs.end()) {
        stream.Printf(
            "  WARNING: REL32 trampoline skipped (unresolved symbol '%s')\n",
            sym.str().c_str());
        continue;
      }

      addr_t sta = tramp_base + ti * kRel32TrampSize;
      uint8_t instr[kRel32TrampSize];
      instr[0] = 0x49;
      instr[1] = 0xBB;
      memcpy(instr + 2, &sit->second, sizeof(addr_t));
      instr[10] = 0x41;
      instr[11] = 0xFF;
      instr[12] = 0xE3;
      instr[13] = 0xCC;
      instr[14] = 0xCC;
      instr[15] = 0xCC;

      Status we;
      process_sp->WriteMemory(sta, instr, sizeof(instr), we);
      if (we.Fail()) {
        stream.Printf("  WARNING: failed to write REL32 trampoline for '%s'\n",
                      sym.str().c_str());
        continue;
      }
      rel32_tramp_addr[sidx][sym] = sta;
    }
  }

  // Apply COFF relocations.
  sec_idx = 0;
  for (const auto &sec : coff->sections()) {
    ++sec_idx;
    if (!section_map.count(sec_idx))
      continue;

    const SectionInfo &si = section_map[sec_idx];

    for (const auto &reloc : sec.relocations()) {
      llvm::object::symbol_iterator sym_it = reloc.getSymbol();
      auto sym_name_or_err = sym_it->getName();
      if (!sym_name_or_err) {
        llvm::consumeError(sym_name_or_err.takeError());
        continue;
      }

      llvm::StringRef sym_name = *sym_name_or_err;
      auto it = symbol_addrs.find(sym_name);
      if (it == symbol_addrs.end()) {
        stream.Printf("  WARNING: relocation to unresolved symbol '%s'\n",
                      sym_name.str().c_str());
        continue;
      }

      addr_t sym_addr = it->second;
      uint64_t offset = reloc.getOffset();
      addr_t patch_addr = si.debuggee_addr + offset;
      uint64_t reloc_type = reloc.getType();

      Status write_err;
      if (reloc_type == llvm::COFF::IMAGE_REL_AMD64_REL32 ||
          reloc_type == llvm::COFF::IMAGE_REL_AMD64_REL32_1 ||
          reloc_type == llvm::COFF::IMAGE_REL_AMD64_REL32_2 ||
          reloc_type == llvm::COFF::IMAGE_REL_AMD64_REL32_3 ||
          reloc_type == llvm::COFF::IMAGE_REL_AMD64_REL32_4 ||
          reloc_type == llvm::COFF::IMAGE_REL_AMD64_REL32_5) {
        // PC-relative 32-bit relocation.
        uint32_t addend_offset = 0;
        if (reloc_type == llvm::COFF::IMAGE_REL_AMD64_REL32_1)
          addend_offset = 1;
        else if (reloc_type == llvm::COFF::IMAGE_REL_AMD64_REL32_2)
          addend_offset = 2;
        else if (reloc_type == llvm::COFF::IMAGE_REL_AMD64_REL32_3)
          addend_offset = 3;
        else if (reloc_type == llvm::COFF::IMAGE_REL_AMD64_REL32_4)
          addend_offset = 4;
        else if (reloc_type == llvm::COFF::IMAGE_REL_AMD64_REL32_5)
          addend_offset = 5;

        // rel32 = target - (patch_addr + 4 + addend_offset)
        addr_t reloc_target = sym_addr;
        int64_t rel_value =
            (int64_t)reloc_target - (int64_t)(patch_addr + 4 + addend_offset);
        if (rel_value > INT32_MAX || rel_value < INT32_MIN) {
          auto tr_it = rel32_tramp_addr.find(sec_idx);
          if (tr_it == rel32_tramp_addr.end()) {
            stream.Printf("  WARNING: REL32 overflow for symbol '%s' (no "
                          "trampoline pool)\n",
                          sym_name.str().c_str());
            continue;
          }
          auto ts_it = tr_it->second.find(sym_name);
          if (ts_it == tr_it->second.end()) {
            stream.Printf("  WARNING: REL32 overflow for symbol '%s' (no "
                          "stub)\n",
                          sym_name.str().c_str());
            continue;
          }
          reloc_target = ts_it->second;
          rel_value = (int64_t)reloc_target -
                      (int64_t)(patch_addr + 4 + addend_offset);
          if (rel_value > INT32_MAX || rel_value < INT32_MIN) {
            stream.Printf("  WARNING: REL32 trampoline for '%s' not reachable "
                          "from relocation site\n",
                          sym_name.str().c_str());
            continue;
          }
        }
        int32_t rel32 = (int32_t)rel_value;
        process_sp->WriteMemory(patch_addr, &rel32, 4, write_err);
      } else if (reloc_type == llvm::COFF::IMAGE_REL_AMD64_ADDR64) {
        uint64_t abs_addr = sym_addr;
        process_sp->WriteMemory(patch_addr, &abs_addr, 8, write_err);
      } else if (reloc_type == llvm::COFF::IMAGE_REL_AMD64_ADDR32NB) {
        // 32-bit relative to image base. We don't have a standard image base
        // for JIT'd code, so use the absolute address and hope it fits.
        uint32_t addr32 = (uint32_t)sym_addr;
        process_sp->WriteMemory(patch_addr, &addr32, 4, write_err);
      } else if (reloc_type == llvm::COFF::IMAGE_REL_AMD64_SECREL) {
        // 32-bit offset from start of section. Used in debug info; skip.
      } else if (reloc_type == llvm::COFF::IMAGE_REL_AMD64_SECTION) {
        // 16-bit section index. Used in debug info; skip.
      } else {
        stream.Printf("  WARNING: unhandled relocation type %llu for '%s'\n",
                      (unsigned long long)reloc_type,
                      sym_name.str().c_str());
      }
    }
  }

  // Find the deoptimized function's address.
  auto fn_it = symbol_addrs.find(unopt_function_name);
  if (fn_it == symbol_addrs.end())
    return Status("deoptimized function '" + unopt_function_name.str() +
                  "' not found in loaded symbols");
  unopt_addr = fn_it->second;

  stream.Printf("  Deoptimized function at 0x%llx\n",
                (unsigned long long)unopt_addr);
  return Status();
}

// ── Function patching ───────────────────────────────────────────────────────

Status DynDbgDeoptimizer::PatchFunctionEntry(lldb::addr_t orig_addr,
                                             lldb::addr_t target_addr,
                                             DynDbgPatchInfo &patch_info) {
  ProcessSP process_sp = m_target.GetProcessSP();
  if (!process_sp)
    return Status("no active process");

  constexpr size_t kJmpRel32Size = 5;
  constexpr size_t kIndirectTrampSize = 12; // movabs rax, imm64; jmp rax

  int64_t rel_direct =
      (int64_t)target_addr - (int64_t)(orig_addr + kJmpRel32Size);

  if (rel_direct <= INT32_MAX && rel_direct >= INT32_MIN) {
    // Single JMP rel32 from the function entry to the deoptimized code.
    patch_info.OriginalBytes.resize(kJmpRel32Size);
    Status read_error;
    process_sp->ReadMemory(orig_addr, patch_info.OriginalBytes.data(),
                           kJmpRel32Size, read_error);
    if (read_error.Fail())
      return Status(std::string("cannot read original function bytes: ") +
                    (read_error.AsCString() ? read_error.AsCString() : ""));

    uint8_t jmp_instr[kJmpRel32Size];
    jmp_instr[0] = 0xE9;
    int32_t rel32 = (int32_t)rel_direct;
    memcpy(jmp_instr + 1, &rel32, 4);

    Status write_error;
    size_t written = process_sp->WriteMemory(orig_addr, jmp_instr,
                                             kJmpRel32Size, write_error);
    if (write_error.Fail() || written != kJmpRel32Size)
      return Status(std::string("cannot patch function entry: ") +
                    (write_error.AsCString() ? write_error.AsCString() : ""));

    patch_info.OriginalAddr = orig_addr;
    patch_info.UnoptimizedAddr = target_addr;
    return Status();
  }

  // JIT / AllocateMemory is often far from the image; rel32 cannot reach.
  // Use a 12-byte indirect trampoline in the NOP pad *before* the entry
  // (lld-link /FUNCTIONPADMIN:N with hotpatchable objects), then JMP rel32
  // from the entry to that trampoline (short negative offset).
  const lldb::addr_t tramp_addr = orig_addr - kIndirectTrampSize;
  const size_t kTotalPatchBytes = kIndirectTrampSize + kJmpRel32Size;

  patch_info.OriginalBytes.resize(kTotalPatchBytes);
  Status read_error;
  process_sp->ReadMemory(tramp_addr, patch_info.OriginalBytes.data(),
                         kTotalPatchBytes, read_error);
  if (read_error.Fail())
    return Status(std::string("cannot read padding + function entry: ") +
                  (read_error.AsCString() ? read_error.AsCString() : ""));

  for (size_t i = 0; i < kIndirectTrampSize; ++i) {
    uint8_t b = patch_info.OriginalBytes[i];
    if (b != 0x90 && b != 0xCC) {
      return Status(
          "deoptimized code is too far for a single JMP (rel32 overflow). "
          "Need at least 12 bytes of NOP (0x90) or INT3 (0xCC) padding "
          "immediately before the function for an indirect trampoline — "
          "use a hotpatchable compile (e.g. clang-cl /hotpatch) and "
          "lld-link /FUNCTIONPADMIN:14 (or larger).");
    }
  }

  // mov rax, target_addr  ->  48 B8 imm64
  // jmp rax                ->  FF E0
  uint8_t tramp[kIndirectTrampSize];
  tramp[0] = 0x48;
  tramp[1] = 0xB8;
  memcpy(tramp + 2, &target_addr, sizeof(target_addr));
  tramp[10] = 0xFF;
  tramp[11] = 0xE0;

  Status write_error;
  size_t w1 = process_sp->WriteMemory(tramp_addr, tramp, kIndirectTrampSize,
                                      write_error);
  if (write_error.Fail() || w1 != kIndirectTrampSize)
    return Status(std::string("cannot write indirect trampoline: ") +
                  (write_error.AsCString() ? write_error.AsCString() : ""));

  int64_t rel_to_tramp =
      (int64_t)tramp_addr - (int64_t)(orig_addr + kJmpRel32Size);
  if (rel_to_tramp > INT32_MAX || rel_to_tramp < INT32_MIN)
    return Status("internal error: trampoline not in rel32 range of entry");

  uint8_t jmp_entry[kJmpRel32Size];
  jmp_entry[0] = 0xE9;
  int32_t rel32 = (int32_t)rel_to_tramp;
  memcpy(jmp_entry + 1, &rel32, 4);

  size_t w2 = process_sp->WriteMemory(orig_addr, jmp_entry, kJmpRel32Size,
                                      write_error);
  if (write_error.Fail() || w2 != kJmpRel32Size)
    return Status(std::string("cannot patch function entry: ") +
                  (write_error.AsCString() ? write_error.AsCString() : ""));

  patch_info.OriginalAddr = tramp_addr;
  patch_info.UnoptimizedAddr = target_addr;

  return Status();
}

Status
DynDbgDeoptimizer::UnpatchFunctionEntry(const DynDbgPatchInfo &patch_info) {
  ProcessSP process_sp = m_target.GetProcessSP();
  if (!process_sp)
    return Status("no active process");

  Status write_error;
  size_t written = process_sp->WriteMemory(
      patch_info.OriginalAddr, patch_info.OriginalBytes.data(),
      patch_info.OriginalBytes.size(), write_error);
  if (write_error.Fail() || written != patch_info.OriginalBytes.size())
    return Status("cannot restore original function bytes");

  return Status();
}

// ── TU hash extraction ──────────────────────────────────────────────────────

std::string DynDbgDeoptimizer::ExtractTUHash(llvm::StringRef pdb_path) {
  constexpr llvm::StringRef tag = ".dyndbg.";

  std::unique_ptr<llvm::pdb::IPDBSession> session;
  if (auto err = llvm::pdb::loadDataForPDB(llvm::pdb::PDB_ReaderType::Native,
                                           pdb_path, session)) {
    llvm::consumeError(std::move(err));
    return "";
  }

  auto *ns = static_cast<llvm::pdb::NativeSession *>(session.get());
  llvm::pdb::PDBFile &file = ns->getPDBFile();

  auto exp_publics = file.getPDBPublicsStream();
  if (!exp_publics) {
    llvm::consumeError(exp_publics.takeError());
    return "";
  }
  auto exp_symbols = file.getPDBSymbolStream();
  if (!exp_symbols) {
    llvm::consumeError(exp_symbols.takeError());
    return "";
  }

  const auto &table = exp_publics->getPublicsTable();
  for (uint32_t off : table) {
    llvm::codeview::CVSymbol sym = exp_symbols->readRecord(off);
    if (sym.kind() != llvm::codeview::SymbolKind::S_PUB32)
      continue;
    llvm::codeview::PublicSym32 pub(
        llvm::codeview::SymbolRecordKind::PublicSym32);
    if (auto err = llvm::codeview::SymbolDeserializer::deserializeAs<
            llvm::codeview::PublicSym32>(sym, pub)) {
      llvm::consumeError(std::move(err));
      continue;
    }
    size_t pos = pub.Name.find(tag);
    if (pos != llvm::StringRef::npos)
      return pub.Name.substr(pos + tag.size()).str();
  }
  return "";
}

// ── Build info lookup ───────────────────────────────────────────────────────

Status DynDbgDeoptimizer::GetBuildInfoForFunction(
    llvm::StringRef function_name, DynDbgBuildInfo &build_info,
    lldb::addr_t &function_addr) {
  // Find the function's address in the target.
  SymbolContextList sc_list;
  ModuleFunctionSearchOptions opts;
  opts.include_inlines = false;
  opts.include_symbols = true;
  m_target.GetImages().FindFunctions(ConstString(function_name),
                                     eFunctionNameTypeFull, opts, sc_list);

  // Fallback: try as a partial/base name.
  if (sc_list.GetSize() == 0) {
    m_target.GetImages().FindFunctions(ConstString(function_name),
                                       eFunctionNameTypeBase, opts, sc_list);
  }

  if (sc_list.GetSize() == 0)
    return Status("function '" + function_name.str() + "' not found in target");

  SymbolContext sc;
  sc_list.GetContextAtIndex(0, sc);

  if (sc.function)
    function_addr = sc.function->GetAddress().GetLoadAddress(&m_target);
  else if (sc.symbol)
    function_addr = sc.symbol->GetLoadAddress(&m_target);
  else
    return Status("cannot determine address for '" + function_name.str() + "'");

  if (function_addr == LLDB_INVALID_ADDRESS)
    return Status("invalid load address for '" + function_name.str() + "'");

  // Get the PDB path from the module.
  ModuleSP module_sp = sc.module_sp;
  // FindFunctions can return a SymbolContext with a valid address but no
  // module on Windows (e.g. CodeView / PDB-backed symbols). Resolve the
  // owning image from the load address.
  if (!module_sp) {
    Address so_addr;
    if (m_target.ResolveLoadAddress(function_addr, so_addr))
      module_sp = so_addr.GetModule();
  }
  if (!module_sp)
    return Status("no module for '" + function_name.str() + "'");

  FileSpec sym_spec = GetNativePDBSpec(module_sp);
  if (!sym_spec)
    return Status("no PDB found for module containing '" +
                  function_name.str() + "'");

  std::string pdb_path = sym_spec.GetPath();

  // Open the PDB and extract LF_BUILDINFO for the compiland containing
  // the function.
  std::unique_ptr<llvm::pdb::IPDBSession> session;
  if (auto err = llvm::pdb::loadDataForPDB(llvm::pdb::PDB_ReaderType::Native,
                                           pdb_path, session))
    return Status("cannot open PDB " + pdb_path + ": " +
                  llvm::toString(std::move(err)));

  auto *ns = static_cast<llvm::pdb::NativeSession *>(session.get());
  llvm::pdb::PDBFile &file = ns->getPDBFile();

  auto exp_dbi = file.getPDBDbiStream();
  if (!exp_dbi)
    return Status("cannot read DBI stream: " +
                  llvm::toString(exp_dbi.takeError()));

  auto exp_ipi = file.getPDBIpiStream();
  if (!exp_ipi)
    return Status("cannot read IPI stream: " +
                  llvm::toString(exp_ipi.takeError()));

  auto &ipi_types = exp_ipi->typeCollection();
  const auto &modules = exp_dbi->modules();
  uint32_t mod_count = modules.getModuleCount();

  for (uint32_t i = 0; i < mod_count; ++i) {
    auto desc = modules.getModuleDescriptor(i);
    uint16_t stream_idx = desc.getModuleStreamIndex();
    if (stream_idx == llvm::pdb::kInvalidStreamIndex)
      continue;

    auto stream_data = file.createIndexedStream(stream_idx);
    if (!stream_data)
      continue;

    llvm::pdb::ModuleDebugStreamRef debug_stream(desc,
                                                  std::move(stream_data));
    if (auto err = debug_stream.reload()) {
      llvm::consumeError(std::move(err));
      continue;
    }

    const auto &sym_array = debug_stream.getSymbolArray();
    if (!moduleContainsFunction(sym_array, function_name))
      continue;

    auto bi = extractBuildInfo(file, ipi_types, sym_array);
    if (!bi)
      continue;

    if (bi->CompilerPath.empty() ||
        bi->CommandLine.find("-cc1") == std::string::npos)
      continue;

    build_info = *bi;

    // Resolve .obj path from DBI module descriptor.
    llvm::StringRef obj_name = desc.getObjFileName();
    llvm::StringRef mod_name = desc.getModuleName();
    if (obj_name.ends_with(".lib") || obj_name.ends_with(".a")) {
      llvm::SmallString<256> resolved(bi->WorkingDir);
      llvm::sys::path::append(resolved, mod_name);
      build_info.ObjFilePath = std::string(resolved);
    } else {
      build_info.ObjFilePath = obj_name.str();
    }

    return Status();
  }

  return Status("no PDB module with LF_BUILDINFO found for '" +
                function_name.str() + "'");
}

// ── Main deoptimize/reoptimize entry points ─────────────────────────────────

Status DynDbgDeoptimizer::Deoptimize(llvm::StringRef function_name,
                                     Stream &stream) {
  stream.Printf("Deoptimizing function '%s'...\n",
                function_name.str().c_str());

  // Step 1: Find the function and its build info from PDB.
  auto t1 = Clock::now();
  DynDbgBuildInfo build_info;
  addr_t function_addr = LLDB_INVALID_ADDRESS;
  Status status =
      GetBuildInfoForFunction(function_name, build_info, function_addr);
  if (status.Fail())
    return status;
  auto t2 = Clock::now();

  stream.Printf("  [1/5] PDB lookup: %.1f ms\n", elapsedMs(t1, t2));
  stream.Printf("    Source: %s\n", build_info.SourceFile.c_str());
  stream.Printf("    ObjFile: %s\n", build_info.ObjFilePath.c_str());
  stream.Printf("    Function addr: 0x%llx\n",
                (unsigned long long)function_addr);

  // Step 2: Extract bitcode from .obj.
  auto t3 = Clock::now();
  std::string bc_error;
  auto bc_buf = ExtractBitcodeFromObj(build_info.ObjFilePath, bc_error);
  if (!bc_buf)
    return Status(bc_error);
  auto t4 = Clock::now();
  stream.Printf("  [2/5] Bitcode extraction: %.1f ms (%.1f KB)\n",
                elapsedMs(t3, t4), bc_buf->getBufferSize() / 1024.0);

  // Extract TU hash for symbol name mangling (same PDB path rules as PDB lookup).
  std::string tu_hash;
  ModuleSP pdb_module;
  {
    Address so_addr;
    if (m_target.ResolveLoadAddress(function_addr, so_addr))
      pdb_module = so_addr.GetModule();
  }
  if (FileSpec pdb_spec = GetNativePDBSpec(pdb_module); pdb_spec)
    tu_hash = ExtractTUHash(pdb_spec.GetPath());

  // Step 3: Thin bitcode to single function.
  auto t5 = Clock::now();
  llvm::SmallString<256> thin_bc_path;
  llvm::sys::path::system_temp_directory(/*ErasedOnReboot=*/true, thin_bc_path);
  llvm::sys::path::append(
      thin_bc_path,
      llvm::sys::path::stem(build_info.SourceFile).str() + ".thin.bc");

  status = ThinBitcode(*bc_buf, function_name, tu_hash, thin_bc_path, stream);
  if (status.Fail())
    return status;
  auto t6 = Clock::now();
  stream.Printf("  [3/5] Bitcode thinning: %.1f ms\n", elapsedMs(t5, t6));

  // Step 4: Codegen at -O0.
  auto t7 = Clock::now();
  llvm::SmallString<256> output_obj_path;
  llvm::sys::path::system_temp_directory(/*ErasedOnReboot=*/true,
                                         output_obj_path);
  llvm::sys::path::append(
      output_obj_path,
      llvm::sys::path::stem(build_info.SourceFile).str() + ".dyndbg.obj");

  status =
      RunCodegen(build_info, thin_bc_path, output_obj_path, stream);
  if (status.Fail())
    return status;
  auto t8 = Clock::now();
  stream.Printf("  [4/5] Codegen: %.1f ms\n", elapsedMs(t7, t8));

  // Step 5: Load .obj into debuggee and patch function entry.
  auto t9 = Clock::now();
  std::string unopt_name =
      (function_name + ".dyndbg.unopt").str();
  addr_t unopt_addr = LLDB_INVALID_ADDRESS;
  DynDbgPatchInfo patch_info;
  patch_info.FunctionName = function_name.str();

  status = LoadObjIntoDebuggee(output_obj_path, unopt_name, unopt_addr,
                               patch_info, stream);
  if (status.Fail())
    return status;

  status = PatchFunctionEntry(function_addr, unopt_addr, patch_info);
  if (status.Fail())
    return status;
  auto t10 = Clock::now();
  stream.Printf("  [5/5] Load + patch: %.1f ms\n", elapsedMs(t9, t10));

  m_patches[function_name] = std::move(patch_info);

  double total_ms = elapsedMs(t1, t10);
  stream.Printf("  DONE: '%s' deoptimized in %.1f ms "
                "(0x%llx -> 0x%llx)\n",
                function_name.str().c_str(), total_ms,
                (unsigned long long)function_addr,
                (unsigned long long)unopt_addr);

  return Status();
}

Status DynDbgDeoptimizer::Reoptimize(llvm::StringRef function_name,
                                     Stream &stream) {
  auto it = m_patches.find(function_name);
  if (it == m_patches.end())
    return Status("function '" + function_name.str() +
                  "' is not currently deoptimized");

  Status status = UnpatchFunctionEntry(it->second);
  if (status.Fail())
    return status;

  stream.Printf("Restored optimized entry for '%s' at 0x%llx\n",
                function_name.str().c_str(),
                (unsigned long long)it->second.OriginalAddr);

  // TODO: Deallocate loaded code/data sections.
  m_patches.erase(it);
  return Status();
}

Status DynDbgDeoptimizer::ReoptimizeAll(Stream &stream) {
  if (m_patches.empty()) {
    stream.Printf("No functions are currently deoptimized.\n");
    return Status();
  }

  std::vector<std::string> names;
  for (const auto &kv : m_patches)
    names.push_back(kv.first().str());

  for (const auto &name : names) {
    Status status = Reoptimize(name, stream);
    if (status.Fail())
      return status;
  }

  return Status();
}

void DynDbgDeoptimizer::PrintStatus(Stream &stream) const {
  if (m_patches.empty()) {
    stream.Printf("No functions are currently deoptimized.\n");
    return;
  }

  stream.Printf("Deoptimized functions:\n");
  for (const auto &kv : m_patches) {
    const DynDbgPatchInfo &pi = kv.second;
    stream.Printf("  %s: 0x%llx -> 0x%llx\n", kv.first().str().c_str(),
                  (unsigned long long)pi.OriginalAddr,
                  (unsigned long long)pi.UnoptimizedAddr);
  }
}

bool DynDbgDeoptimizer::IsDeoptimized(llvm::StringRef function_name) const {
  return m_patches.count(function_name) > 0;
}
