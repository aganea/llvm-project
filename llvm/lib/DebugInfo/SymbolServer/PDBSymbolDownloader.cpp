//===- PDBSymbolDownloader.cpp - PDB Symbol Download Support ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/SymbolServer/PDBSymbolDownloader.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/DebugInfo/CodeView/CVRecord.h"
#include "llvm/DebugInfo/CodeView/Formatters.h"
#include "llvm/DebugInfo/CodeView/SymbolDeserializer.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/GlobalsStream.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/PublicsStream.h"
#include "llvm/DebugInfo/PDB/Native/SymbolStream.h"
#include "llvm/DebugInfo/PDB/PDB.h"
#include "llvm/DebugInfo/PDB/PDBSymbol.h"
#include "llvm/DebugInfo/PDB/PDBSymbolExe.h"
#include "llvm/DebugInfo/PDB/PDBSymbolFunc.h"
#include "llvm/Demangle/MicrosoftDemangle.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/HTTP/HTTPClient.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Windows/WindowsSupport.h"
#include "llvm/Support/raw_ostream.h"

#include <Psapi.h>
#include <windows.h>

using namespace llvm;
using namespace llvm::pdb;
using namespace llvm::object;

/// Helper to download file using HTTPClient
class FileDownloadHandler : public HTTPResponseHandler {
private:
  std::error_code EC;
  raw_fd_ostream Stream;

public:
  FileDownloadHandler(StringRef FilePath) : Stream(FilePath.str(), EC) {
    if (EC) {
      EC = std::make_error_code(std::errc::io_error);
    }
  }
  virtual ~FileDownloadHandler() = default;

  Error handleBodyChunk(StringRef BodyChunk) override {
    if (EC)
      return createStringError(std::make_error_code(std::errc::io_error),
                               "Failed to open file for writing");
    if (Stream.has_error())
      return createStringError(std::make_error_code(std::errc::io_error),
                               "Error writing to file");

    Stream.write(BodyChunk.data(), BodyChunk.size());

    if (Stream.has_error())
      return createStringError(std::make_error_code(std::errc::io_error),
                               "Error writing to file");

    return Error::success();
  }
};

/// Helper to download file from URL using HTTPClient
static Expected<std::string> DownloadFileViaHTTP(StringRef Url,
                                                 StringRef OutputPath) {
  if (!HTTPClient::isAvailable())
    return createStringError(std::make_error_code(std::errc::not_supported),
                             "HTTP client is not available");
  HTTPRequest Request(Url);
  Request.FollowRedirects = true;

  FileDownloadHandler Handler(OutputPath);
  HTTPClient Client;
  Client.setTimeout(std::chrono::seconds(60)); // 60-sec timeout

  if (Error Err = Client.perform(Request, Handler))
    return std::move(Err);

  unsigned ResponseCode = Client.responseCode();
  if (ResponseCode != 200) {
    return createStringError(std::make_error_code(std::errc::io_error),
                             "HTTP request failed with status code " +
                                 std::to_string(ResponseCode));
  }

  return OutputPath.str();
}

// In Process.inc
void getSystemModule(LPCWSTR ModuleName, SmallVectorImpl<wchar_t> &ModulePath);

namespace llvm::symbolserver {

PDBSymbolDownloader::PDBSymbolDownloader() {
  // Initialize symbols cache directory on construction
  if (auto CacheDir = getSymbolsCacheDirectory()) {
    // Success - directory created or already exists
  } else {
    consumeError(CacheDir.takeError());
    // Cache directory creation failed, but continue anyway
  }

  // Ensures that some modules are tagged as system modules. This is triggering
  // a more securing runtime module retrival, to ensure we're not loading a
  // injected malicious module.
  SystemModules.insert("ntdll.dll");
  SystemModules.insert("kernel32.dll");
  SystemModules.insert("mscoree.dll");
  SystemModules.insert("ole32.dll");
  SystemModules.insert("winhttp.dll");
  SystemModules.insert("advapi32.dll");
  SystemModules.insert("oleaut32.dll");
  SystemModules.insert("shell32.dll");
}

Expected<std::string> PDBSymbolDownloader::getSymbolsCacheDirectory() {
#ifdef _WIN32
  // Get the current executable path
  wchar_t ExePath[MAX_PATH];
  DWORD Length = ::GetModuleFileNameW(NULL, ExePath, MAX_PATH);
  if (Length == 0 || Length == MAX_PATH)
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "Failed to get executable path");

  SmallString<MAX_PATH> ExeDir(ExePath, ExePath + Length);
  sys::path::remove_filename(ExeDir);
  sys::path::append(ExeDir, "symbols");

  std::error_code EC = sys::fs::create_directories(ExeDir);
  if (EC)
    return createStringError(EC, "Failed to create symbols directory");

  return ExeDir.str().str();
#else  // _WIN32
  // On non-Windows platforms, use a default cache location
  SmallString<256> CacheDir;
  sys::path::home_directory(CacheDir);
  sys::path::append(CacheDir, ".llvm-symbols");

  std::error_code EC = sys::fs::create_directories(CacheDir);
  if (EC)
    return createStringError(EC, "Failed to create symbols cache directory");

  return CacheDir.str().str();
#endif // _WIN32
}

Expected<MemoryBufferRef>
PDBSymbolDownloader::findModuleBaseAddress(StringRef ModuleName) {
  // Convert module name to wide string
  SmallVector<wchar_t, MAX_PATH> ModuleNameW;
  if (auto EC = llvm::sys::windows::UTF8ToUTF16(ModuleName.str(), ModuleNameW))
    return createStringError(EC, "Failed to convert module name");

  std::string ModuleNameL;
  llvm::transform(ModuleNameL, ModuleNameL.begin(), std::tolower);
  if (SystemModules.count(ModuleNameL)) {
    SmallVector<wchar_t, MAX_PATH> SystemModulePath;
    getSystemModule(ModuleNameW.data(), SystemModulePath);
    ModuleNameW = SystemModulePath;
  }

  // Try to get the module handle
  HMODULE ModuleHandle = ::GetModuleHandleW(ModuleNameW.data());
  if (!ModuleHandle)
    return createStringError(
        std::make_error_code(std::errc::no_such_file_or_directory),
        "module not found: " + ModuleName);

  MODULEINFO Info{};
  if (!::GetModuleInformation(GetCurrentProcess(), ModuleHandle, &Info,
                              sizeof(MODULEINFO)))
    return createStringError(
        std::make_error_code(std::errc::no_such_file_or_directory),
        "cannot query module size: " + ModuleName);

  return MemoryBufferRef(
      StringRef(reinterpret_cast<const char *>(Info.lpBaseOfDll),
                Info.SizeOfImage),
      ModuleName);
}

Expected<std::pair<std::string, std::string>>
PDBSymbolDownloader::extractPDBGuidAge(const COFFObjectFile *CoffObj) {
  if (!CoffObj)
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "COFF object is null");

  // Get PDB info using COFFObjectFile's method
  const codeview::DebugInfo *PdbInfo = nullptr;
  StringRef PdbFileName;
  if (Error E = CoffObj->getDebugPDBInfo(PdbInfo, PdbFileName))
    return std::move(E);

  if (!PdbInfo)
    return createStringError(
        std::make_error_code(std::errc::no_such_file_or_directory),
        "No PDB debug info found");

  // Format as GUID-Age string
  if (PdbInfo->Signature.CVSignature == OMF::Signature::PDB70) {
    SmallString<40> Result;
    raw_svector_ostream OS(Result);
    OS << formatv("{0}", llvm::codeview::fmt_guid(PdbInfo->PDB70.Signature));
    // Trim the brackets
    std::memmove(Result.begin(), Result.begin() + 1, Result.size() - 2);
    Result.truncate(Result.size() - 2);
    OS << formatv("-{0}", PdbInfo->PDB70.Age);

    return make_pair(Result.str().str(), PdbFileName.str());
  }

  return createStringError(std::make_error_code(std::errc::invalid_argument),
                           "Unsupported PDB format");
}

Expected<std::string> PDBSymbolDownloader::downloadPDB(StringRef PdbGuid,
                                                       StringRef PdbPath) {
  // First check if we have the original PDB filepath.
  if (sys::fs::exists(PdbPath)) {
    uint64_t Size;
    if (!sys::fs::file_size(PdbPath, Size) && Size > 0)
      return PdbPath.str();
  }

  // FIXME: check next to the binary path

  auto CacheDirResult = getSymbolsCacheDirectory();
  if (!CacheDirResult)
    return CacheDirResult.takeError();

  std::string Guid = PdbGuid.str();
  // Remove hyphens from GUID for Microsoft symbol server
  llvm::erase(Guid, '-');

  StringRef PdbName = llvm::sys::path::filename(PdbPath);

  SmallString<MAX_PATH> CachedPdbPath(*CacheDirResult);
  sys::path::append(CachedPdbPath, PdbName);
  sys::path::append(CachedPdbPath, Guid);

  std::error_code EC = sys::fs::create_directories(CachedPdbPath);
  if (EC)
    return createStringError(EC, "Failed to create PDB cache directory");

  sys::path::append(CachedPdbPath, PdbName);

  // Check if already cached
  if (sys::fs::exists(CachedPdbPath)) {
    uint64_t Size;
    if (!sys::fs::file_size(CachedPdbPath, Size) && Size > 0)
      return CachedPdbPath.str().str();
  }

  std::string Url = "http://msdl.microsoft.com/download/symbols/" +
                    PdbName.str() + "/" + Guid + "/" + PdbName.str();

  // Download using HTTPClient
  auto DownloadResult = DownloadFileViaHTTP(Url, CachedPdbPath.str());
  if (!DownloadResult)
    return DownloadResult.takeError();

  return CachedPdbPath.str().str();
}

Expected<NativeSession *>
PDBSymbolDownloader::getOrLoadPDBSession(StringRef ModuleName,
                                         const COFFObjectFile *CoffObj) {
  // Check if session is already cached
  auto CacheIt = SessionCache.find(ModuleName);
  if (CacheIt != SessionCache.end())
    return CacheIt->second.get();

  // Extract PDB GUID from DLL
  auto R = extractPDBGuidAge(CoffObj);
  if (!R)
    return R.takeError();

  std::string PdbGuid = R->first;
  std::string PdbPath = R->second;

  // Download PDB from Microsoft symbol servers
  auto PdbPathResult = downloadPDB(PdbGuid, PdbPath);
  if (!PdbPathResult)
    return PdbPathResult.takeError();

  // Load PDB file
  auto Buffer = MemoryBuffer::getFile(*PdbPathResult);
  if (!Buffer)
    return errorCodeToError(Buffer.getError());

  // Create a NativeSession from the PDB buffer
  std::unique_ptr<IPDBSession> ISession;
  if (auto EC = NativeSession::createFromPdb(std::move(*Buffer), ISession))
    return std::move(EC);

  NativeSession *SessionPtr = static_cast<NativeSession *>(ISession.get());

  SessionCache[ModuleName].reset(SessionPtr);
  ISession.release();

  return SessionPtr;
}

Expected<uint64_t>
PDBSymbolDownloader::resolveSymbolInSession(NativeSession *Session,
                                            StringRef SymbolName) {
  // Get the PDB file
  PDBFile &PdbFile = Session->getPDBFile();

  // Try to get the publics stream
  auto PublicsStreamOrErr = PdbFile.getPDBPublicsStream();
  if (!PublicsStreamOrErr)
    return createStringError(
        std::make_error_code(std::errc::no_such_file_or_directory),
        "Failed to get publics stream from PDB");

  PublicsStream &PublicsStream = *PublicsStreamOrErr;

  // Get the symbol stream
  auto SymbolStreamOrErr = PdbFile.getPDBSymbolStream();
  if (!SymbolStreamOrErr)
    return createStringError(
        std::make_error_code(std::errc::no_such_file_or_directory),
        "Failed to get symbol stream from PDB");

  SymbolStream &SymbolStream = *SymbolStreamOrErr;

  // Get the publics hash table
  const GSIHashTable &PublicsTable = PublicsStream.getPublicsTable();

  // Search through all public symbols
  for (uint32_t SymbolOffset : PublicsTable) {
    codeview::CVSymbol Symbol = SymbolStream.readRecord(SymbolOffset);

    // Check if this is a public symbol (S_PUB32)
    if (Symbol.kind() != codeview::S_PUB32)
      continue;

    // Deserialize the public symbol record
    auto DeserializedOrErr =
        codeview::SymbolDeserializer::deserializeAs<codeview::PublicSym32>(
            Symbol);
    if (!DeserializedOrErr) {
      consumeError(DeserializedOrErr.takeError());
      continue;
    }

    codeview::PublicSym32 PublicSym = *DeserializedOrErr;

    // Check if the name matches
    if (PublicSym.Name == SymbolName) {
      // Get section headers to find the RVA for this segment
      auto DbiOrErr = PdbFile.getPDBDbiStream();
      if (!DbiOrErr) {
        consumeError(DbiOrErr.takeError());
        // Fall back to just the offset if we can't get section info
        return static_cast<uint64_t>(PublicSym.Offset);
      }

      DbiStream &DbiStream = *DbiOrErr;
      auto SectionHeaders = DbiStream.getSectionHeaders();

      // PublicSym.Segment is 1-based, SectionHeaders is 0-based
      if (PublicSym.Segment > 0 &&
          PublicSym.Segment <= static_cast<uint16_t>(SectionHeaders.size())) {
        uint32_t SectionRva =
            SectionHeaders[PublicSym.Segment - 1].VirtualAddress;
        return static_cast<uint64_t>(SectionRva + PublicSym.Offset);
      }

      // Return the offset if segment is invalid
      return static_cast<uint64_t>(PublicSym.Offset);
    }
  }

  return createStringError(
      std::make_error_code(std::errc::no_such_file_or_directory),
      "symbol '" + SymbolName + "' not found in PDB publics");
}

static void *findInExports(const COFFObjectFile *CoffObj,
                           StringRef SymbolName) {
  // Search through exported symbols
  for (const auto &ExportEntry : CoffObj->export_directories()) {
    StringRef ExportName;
    if (Error E = ExportEntry.getSymbolName(ExportName))
      continue;
    if (ExportName.empty())
      continue;

    using namespace llvm::ms_demangle;

    Demangler Demangler;
    std::string_view ExportNameView = (std::string_view)ExportName;
    SymbolNode *Node = Demangler.parse(ExportNameView);
    if (!Node)
      continue;
    if (!Node->Name)
      continue;
    std::string FuncName = Node->Name->toString();

    if (SymbolName == FuncName) {
      uint32_t RVA;
      if (Error E = ExportEntry.getExportRVA(RVA))
        continue;
      uint64_t SymbolAddress = CoffObj->getImageBase() + RVA;
      return reinterpret_cast<void *>(SymbolAddress);
    }
  }
  return nullptr;
}

Expected<void *>
PDBSymbolDownloader::getSymbolAddress(StringRef DecoratedSymbolName) {
  // Parse module-decorated symbol name (e.g., "ntdll.dll!LdrpHandleTLSData")
  size_t BangPos = DecoratedSymbolName.find('!');
  if (BangPos == StringRef::npos)
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "symbol is not decorated: " + DecoratedSymbolName);

  StringRef ModuleName = DecoratedSymbolName.substr(0, BangPos);
  StringRef SymbolName = DecoratedSymbolName.substr(BangPos + 1);

  // Find the runtime module base address
  auto ModuleResult = findModuleBaseAddress(ModuleName);
  if (!ModuleResult)
    return ModuleResult.takeError();

  MemoryBufferRef BufferRef = *ModuleResult;

  // Try to load the module first without going through the PDB
  HMODULE H =
      reinterpret_cast<HMODULE>(const_cast<char *>(BufferRef.getBufferStart()));
  if (auto FnPtr = ::GetProcAddress(H, SymbolName.data()))
    return reinterpret_cast<void *>(FnPtr);

  // Build a view of the module in memory.
  auto BinaryOrErr = createBinary(BufferRef, /*Context=*/nullptr,
                                  /*InitContent=*/true, /*Live=*/true);
  if (!BinaryOrErr)
    return createStringError("cannot create module buffer");

  const COFFObjectFile *CoffObj = dyn_cast<COFFObjectFile>(BinaryOrErr->get());
  if (!CoffObj)
    return createStringError("cannot cast COFF object file");

  // Try by looking in exports and demangling them
  if (void *ExportedFnPtr = findInExports(CoffObj, SymbolName))
    return ExportedFnPtr;

  // Get or load the PDB session
  auto SessionResult = getOrLoadPDBSession(ModuleName, CoffObj);
  if (!SessionResult)
    return SessionResult.takeError();

  NativeSession *Session = *SessionResult;

  // Resolve symbol offset in PDB
  auto SymbolOffsetResult = resolveSymbolInSession(Session, SymbolName);
  if (!SymbolOffsetResult)
    return SymbolOffsetResult.takeError();

  // Calculate absolute address
  uint64_t SymbolAddress = CoffObj->getImageBase() + *SymbolOffsetResult;
  return reinterpret_cast<void *>(SymbolAddress);
}

void PDBSymbolDownloader::clearCache() { SessionCache.clear(); }

} // namespace llvm::symbolserver
