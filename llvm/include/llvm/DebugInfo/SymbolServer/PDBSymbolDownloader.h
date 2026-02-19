//===- PDBSymbolDownloader.h - PDB Symbol Download Support -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides utilities for downloading and caching PDB files,
// similar to dbghelp.dll's symbol resolution capabilities. The
// PDBSymbolDownloader maintains an internal cache of loaded PDB sessions
// to avoid repeated file I/O and parsing overhead.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEBUGINFO_SYMBOLSERVER_PDBSYMBOLDOWNLOADER_H
#define LLVM_DEBUGINFO_SYMBOLSERVER_PDBSYMBOLDOWNLOADER_H

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Error.h"

#include <string>

namespace llvm {
namespace symbolserver {

/// Manages PDB symbol resolution with caching, similar to dbghelp.dll.
///
/// Maintains internal cache of loaded PDB sessions to avoid repeated file I/O
/// and PDB parsing overhead. The cache is keyed by module name and persists
/// across multiple symbol lookups.
///
/// Usage:
///   PDBSymbolDownloader Downloader;
///   auto Addr = Downloader.getSymbolAddress("ntdll.dll!LdrpHandleTLSData");
class PDBSymbolDownloader {
private:
  /// Maps module names (e.g., "ntdll.dll") to cached PDB instances
  StringMap<std::unique_ptr<pdb::NativeSession>> SessionCache;

  /// Records a list of modules that are consideres as "system" modules.
  DenseSet<StringRef> SystemModules;

  /// Helper to find runtime module base address by name
  Expected<MemoryBufferRef> findModuleBaseAddress(StringRef ModuleName);

  /// Helper to extract PDB GUID and the PDB filepath from a loaded DLL module
  Expected<std::pair<std::string, std::string>>
  extractPDBGuidAge(const llvm::object::COFFObjectFile *CoffObj);

  /// Helper to download and cache PDB file from Microsoft servers
  Expected<std::string> downloadPDB(StringRef PdbGuid, StringRef PdbPath);

  /// Helper to load or retrieve cached PDB session
  Expected<pdb::NativeSession *>
  getOrLoadPDBSession(StringRef ModuleName,
                      const llvm::object::COFFObjectFile *CoffObj);

  /// Helper to resolve symbol in a PDB session
  Expected<uint64_t> resolveSymbolInSession(pdb::NativeSession *Session,
                                            StringRef SymbolName);

  /// Get or create the symbols cache directory
  Expected<std::string> getSymbolsCacheDirectory();

public:
  /// Constructor initializes the symbol cache directory.
  PDBSymbolDownloader();

  ~PDBSymbolDownloader() = default;

  // Disable copying; sessions are managed internally
  PDBSymbolDownloader(const PDBSymbolDownloader &) = delete;
  PDBSymbolDownloader &operator=(const PDBSymbolDownloader &) = delete;

  /// Resolve a module-decorated symbol name to its runtime address.
  ///
  /// \param DecoratedSymbolName Module-decorated symbol name in the format
  ///        "module.dll!symbolname" (e.g., "ntdll.dll!LdrpHandleTLSData")
  /// \return Absolute address of the symbol in the current process address
  /// space,
  ///         or error if the module/symbol cannot be found
  ///
  /// This function:
  /// 1. Finds the runtime module base address
  /// 2. Extracts the PDB GUID from the loaded module
  /// 3. Downloads/retrieves the PDB from cache
  /// 4. Resolves the symbol in the PDB
  /// 5. Returns the absolute address
  Expected<void *> getSymbolAddress(StringRef DecoratedSymbolName);

  /// Clear the internal session cache and free resources
  void clearCache();
};

} // namespace symbolserver
} // namespace llvm

#endif // LLVM_DEBUGINFO_SYMBOLSERVER_PDBSYMBOLDOWNLOADER_H