//===- Win32/DynamicLibrary.cpp - Win32 DL Implementation -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides the Win32 specific implementation of DynamicLibrary.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/LLVMDriver.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/Windows/WindowsSupport.h"
#include "llvm/Support/WindowsError.h"
#include "llvm/Support/raw_ostream.h"

#include <psapi.h>
#include <winnt.h>
#include <winternl.h>

#include <thread>

//===----------------------------------------------------------------------===//
//=== WARNING: Implementation here must contain only Win32 specific code
//===          and must not be UNIX code.
//===----------------------------------------------------------------------===//

DynamicLibrary::HandleSet::~HandleSet() {
  for (void *Handle : llvm::reverse(Handles))
    FreeLibrary(HMODULE(Handle));

  // 'Process' should not be released on Windows.
  assert((Process == &Invalid || Process == this) && "Bad Handle");
  // llvm_shutdown called, Return to default
  DynamicLibrary::SearchOrder = DynamicLibrary::SO_Linker;
}

void *DynamicLibrary::HandleSet::DLOpen(const char *File, std::string *Err) {
  // Create the instance and return it to be the *Process* handle
  // simillar to dlopen(NULL, RTLD_LAZY|RTLD_GLOBAL)
  if (!File)
    return &getGlobals().OpenedHandles;

  SmallVector<wchar_t, MAX_PATH> FileUnicode;
  if (std::error_code ec = windows::UTF8ToUTF16(File, FileUnicode)) {
    SetLastError(ec.value());
    MakeErrMsg(Err, std::string(File) + ": Can't convert to UTF-16");
    return &DynamicLibrary::Invalid;
  }

  HMODULE Handle = LoadLibraryW(FileUnicode.data());
  if (Handle == NULL) {
    MakeErrMsg(Err, std::string(File) + ": Can't open");
    return &DynamicLibrary::Invalid;
  }

  return reinterpret_cast<void *>(Handle);
}

static DynamicLibrary::HandleSet *IsOpenedHandlesInstance(void *Handle) {
  DynamicLibrary::HandleSet &Inst = getGlobals().OpenedHandles;
  return Handle == &Inst ? &Inst : nullptr;
}

void DynamicLibrary::HandleSet::DLClose(void *Handle) {
  if (HandleSet *HS = IsOpenedHandlesInstance(Handle))
    HS->Process = &Invalid; // Just drop the *Process* handle.
  else
    FreeLibrary((HMODULE)Handle);
}

static bool GetProcessModules(HANDLE H, DWORD &Bytes, HMODULE *Data = nullptr) {
  // EnumProcessModules will fail on Windows 64 while some versions of
  // MingW-32 don't have EnumProcessModulesEx.
  if (
#ifdef _WIN64
      !EnumProcessModulesEx(H, Data, Bytes, &Bytes, LIST_MODULES_64BIT)
#else
      !EnumProcessModules(H, Data, Bytes, &Bytes)
#endif
  ) {
    std::string Err;
    if (MakeErrMsg(&Err, "EnumProcessModules failure"))
      llvm::errs() << Err << "\n";
    return false;
  }
  return true;
}

void *DynamicLibrary::HandleSet::DLSym(void *Handle, const char *Symbol) {
  HandleSet *HS = IsOpenedHandlesInstance(Handle);
  if (!HS)
    return (void *)uintptr_t(GetProcAddress((HMODULE)Handle, Symbol));

  // Could have done a dlclose on the *Process* handle
  if (HS->Process == &Invalid)
    return nullptr;

  // Trials indicate EnumProcessModulesEx is consistantly faster than using
  // EnumerateLoadedModules64 or CreateToolhelp32Snapshot.
  //
  // | Handles | DbgHelp.dll | CreateSnapshot | EnumProcessModulesEx
  // |=========|=============|========================================
  // | 37      | 0.0000585 * | 0.0003031      | 0.0000152
  // | 1020    | 0.0026310 * | 0.0121598      | 0.0002683
  // | 2084    | 0.0149418 * | 0.0369936      | 0.0005610
  //
  // * Not including the load time of Dbghelp.dll (~.005 sec)
  //
  // There's still a case to somehow cache the result of EnumProcessModulesEx
  // across invocations, but the complication of doing that properly...
  // Possibly using LdrRegisterDllNotification to invalidate the cache?

  DWORD Bytes = 0;
  HMODULE Self = HMODULE(GetCurrentProcess());
  if (!GetProcessModules(Self, Bytes))
    return nullptr;

  // Get the most recent list in case any modules added/removed between calls
  // to EnumProcessModulesEx that gets the amount of, then copies the HMODULES.
  // MSDN is pretty clear that if the module list changes during the call to
  // EnumProcessModulesEx the results should not be used.
  std::vector<HMODULE> Handles;
  do {
    assert(Bytes && ((Bytes % sizeof(HMODULE)) == 0) &&
           "Should have at least one module and be aligned");
    Handles.resize(Bytes / sizeof(HMODULE));
    if (!GetProcessModules(Self, Bytes, Handles.data()))
      return nullptr;
  } while (Bytes != (Handles.size() * sizeof(HMODULE)));

  // Try EXE first, mirroring what dlsym(dlopen(NULL)) does.
  if (FARPROC Ptr = GetProcAddress(HMODULE(Handles.front()), Symbol))
    return (void *)uintptr_t(Ptr);

  if (Handles.size() > 1) {
    // This is different behaviour than what Posix dlsym(dlopen(NULL)) does.
    // Doing that here is causing real problems for the JIT where msvc.dll
    // and ucrt.dll can define the same symbols. The runtime linker will choose
    // symbols from ucrt.dll first, but iterating NOT in reverse here would
    // mean that the msvc.dll versions would be returned.

    for (auto I = Handles.rbegin(), E = Handles.rend() - 1; I != E; ++I) {
      if (FARPROC Ptr = GetProcAddress(HMODULE(*I), Symbol))
        return (void *)uintptr_t(Ptr);
    }
  }
  return nullptr;
}

// Stack probing routines are in the support library (e.g. libgcc), but we don't
// have dynamic linking on windows. Provide a hook.
#define EXPLICIT_SYMBOL(SYM)                                                   \
  extern "C" {                                                                 \
  extern void *SYM;                                                            \
  }
#define EXPLICIT_SYMBOL2(SYMFROM, SYMTO) EXPLICIT_SYMBOL(SYMTO)

#ifdef _M_IX86
// Win32 on x86 implements certain single-precision math functions as macros.
// These functions are not exported by the DLL, but will still be needed
// for symbol-resolution by the JIT loader. Therefore, this Support libray
// provides helper functions with the same implementation.

#define INLINE_DEF_SYMBOL1(TYP, SYM)                                           \
  extern "C" TYP inline_##SYM(TYP _X) { return SYM(_X); }
#define INLINE_DEF_SYMBOL2(TYP, SYM)                                           \
  extern "C" TYP inline_##SYM(TYP _X, TYP _Y) { return SYM(_X, _Y); }
#endif

#include "explicit_symbols.inc"

#undef EXPLICIT_SYMBOL
#undef EXPLICIT_SYMBOL2
#undef INLINE_DEF_SYMBOL1
#undef INLINE_DEF_SYMBOL2

static void *DoSearch(const char *SymbolName) {

#define EXPLICIT_SYMBOL(SYM)                                                   \
  if (!strcmp(SymbolName, #SYM))                                               \
    return (void *)&SYM;
#define EXPLICIT_SYMBOL2(SYMFROM, SYMTO)                                       \
  if (!strcmp(SymbolName, #SYMFROM))                                           \
    return (void *)&SYMTO;

#ifdef _M_IX86
#define INLINE_DEF_SYMBOL1(TYP, SYM)                                           \
  if (!strcmp(SymbolName, #SYM))                                               \
    return (void *)&inline_##SYM;
#define INLINE_DEF_SYMBOL2(TYP, SYM) INLINE_DEF_SYMBOL1(TYP, SYM)
#endif

  {
#include "explicit_symbols.inc"
  }

#undef EXPLICIT_SYMBOL
#undef EXPLICIT_SYMBOL2
#undef INLINE_DEF_SYMBOL1
#undef INLINE_DEF_SYMBOL2

  return nullptr;
}

//===----------------------------------------------------------------------===//
// EXE-as-DLL Loading Support
//
// This section implements the machinery for loading a statically-linked EXE
// into the current process as if it were a DLL.  The Windows loader's
// LoadLibraryW can map an EXE, but it does not perform the full initialization
// that a DLL would receive: the Import Address Table (IAT) is not patched,
// static Thread-Local Storage (TLS) is not allocated, and the CRT is not
// initialized.  The functions below perform each of those steps manually.
//
// The high-level flow is:
//   1. LoadLibraryW         -- map the PE into the address space
//   2. RemapIAT             -- resolve and patch all imported function pointers
//   3. FindEntryPoints      -- locate the module's exported tool entry point
//   4. InitializeStaticTLS  -- call ntdll!LdrpHandleTlsData to register TLS
//   5. InitializeCRT        -- run the module's CRT startup on a helper thread
//   6. SaveTLS              -- snapshot the post-CRT TLS state for later resets
//===----------------------------------------------------------------------===//

/// Tracks the state of a single EXE that has been loaded in-process.
struct ExecModule {
  llvm::StringRef FullPath; ///< Directory containing the module.
  llvm::StringRef Name;     ///< Filename of the loaded module (e.g. "lld.exe").
  HANDLE Handle{};          ///< HMODULE from LoadLibraryW (cast to HANDLE).
  void *TLSInitBuffer{};    ///< Snapshot of static TLS data taken after CRT
                            ///< init; restored before each tool invocation.
  HANDLE ExitEvent{};       ///< Event signaled to let the CRT helper thread
                            ///< terminate cleanly.
  HANDLE TLSThreadHandle{}; ///< Native handle of the suspended CRT helper
                            ///< thread (used during shutdown).
  llvm::ToolSymbolTable ToolSymTab; ///< Entry points exported by the module.

  /// Retrieve the module at 1-based \p ModuleIndex.
  static ExecModule &getModule(int ModuleIndex);
};

/// All modules that have been successfully loaded in this process.
/// NOTE: This container is not thread-safe. All access (loading modules,
/// launching tools) must be serialized on a single thread.
std::vector<ExecModule> LoadedModules;

/// Allocator and saver for strings that must outlive individual function calls
/// (e.g. module names stored in ExecModule).
llvm::BumpPtrAllocator Alloc;
llvm::StringSaver Saver(Alloc);

ExecModule &ExecModule::getModule(int ModuleIndex) {
  assert(ModuleIndex > 0 &&
         ModuleIndex <= static_cast<int>(LoadedModules.size()));
  return LoadedModules[ModuleIndex - 1];
}

/// Overwrite bytes at \p Target in the current process's address space.
///
/// If the target page is read-only, temporarily changes its protection to
/// PAGE_WRITECOPY, writes the data, and restores the original protection.
static std::error_code PatchProcessBytes(void *Target, llvm::StringRef Data) {
  HANDLE Proc = GetCurrentProcess();
  if (WriteProcessMemory(Proc, Target, Data.data(), Data.size(), nullptr))
    return std::error_code();
  if (GetLastError() != ERROR_NOACCESS)
    return mapWindowsError(GetLastError());

  // The page is not writable -- temporarily change protection and retry.
  DWORD OldProtect;
  if (!VirtualProtect(Target, Data.size(), PAGE_WRITECOPY, &OldProtect))
    return make_error_code(std::errc::operation_not_supported);

  bool Ok = WriteProcessMemory(Proc, Target, Data.data(), Data.size(), nullptr);

  VirtualProtect(Target, Data.size(), OldProtect, &OldProtect);

  return Ok ? std::error_code()
            : make_error_code(std::errc::operation_not_supported);
}

/// Retrieve a pointer to a PE data directory entry from a memory-mapped image.
///
/// This is a lightweight alternative to the dbghelp ImageDirectoryEntryToData
/// function, avoiding the dbghelp.lib dependency.  The image must already be
/// mapped as an image (i.e. via LoadLibrary, not MapViewOfFile with
/// SEC_COMMIT), so RVAs can be used directly as offsets from the base.
static void *GetImageDirectoryEntryData(void *BaseOfImage,
                                        USHORT DirectoryIndex, DWORD *Size) {
  if (!BaseOfImage || !Size)
    return nullptr;

  auto *Base = reinterpret_cast<const uint8_t *>(BaseOfImage);

  auto *DosHeader = reinterpret_cast<const IMAGE_DOS_HEADER *>(Base);
  if (DosHeader->e_magic != IMAGE_DOS_SIGNATURE)
    return nullptr;

  auto *NtHeaders =
      reinterpret_cast<const IMAGE_NT_HEADERS *>(Base + DosHeader->e_lfanew);
  if (NtHeaders->Signature != IMAGE_NT_SIGNATURE)
    return nullptr;

  if (DirectoryIndex >= NtHeaders->OptionalHeader.NumberOfRvaAndSizes) {
    *Size = 0;
    return nullptr;
  }

  const IMAGE_DATA_DIRECTORY &DataDir =
      NtHeaders->OptionalHeader.DataDirectory[DirectoryIndex];

  if (DataDir.VirtualAddress == 0 || DataDir.Size == 0) {
    *Size = 0;
    return nullptr;
  }

  *Size = DataDir.Size;
  return const_cast<uint8_t *>(Base + DataDir.VirtualAddress);
}

/// Patch the Import Address Table (IAT) of a loaded PE module.
///
/// When an EXE is loaded via LoadLibraryW, the loader does not resolve its
/// imports (unlike a DLL).  This function walks the PE's import descriptors,
/// loads each referenced DLL, resolves every imported symbol, and patches
/// the IAT entries in-place so that the loaded code calls the correct
/// functions.
///
/// The function also rejects modules linked against the dynamic CRT (/MD or
/// /MDd) because in-process EXE loading requires a statically linked CRT.
static std::error_code RemapIAT(HINSTANCE H) {
  DWORD Size = 0;
  auto *ImportDesc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(
      GetImageDirectoryEntryData(H, IMAGE_DIRECTORY_ENTRY_IMPORT, &Size));
  if (!ImportDesc)
    return make_error_code(std::errc::operation_not_supported);

  auto *BaseBytes = reinterpret_cast<uint8_t *>(H);

  for (; ImportDesc->Name; ImportDesc++) {
    auto *ModName =
        reinterpret_cast<const char *>(BaseBytes + ImportDesc->Name);
    if (!ModName)
      break;

    // Reject modules linked against the dynamic CRT (/MD or /MDd).  We
    // require the CRT to be linked statically so that the loaded EXE shares
    // the host process's CRT state.
    StringRef ModNameRef(ModName);
    if (ModNameRef.starts_with_insensitive("msvc") ||
        ModNameRef.starts_with_insensitive("concrt") ||
        ModNameRef.starts_with_insensitive("vcruntime") ||
        ModNameRef.starts_with_insensitive("ucrtbase"))
      return make_error_code(std::errc::operation_not_supported);

    HINSTANCE ImportDLL = LoadLibraryA(ModName);
    if (!ImportDLL)
      continue;

    // Walk the IAT thunks for this imported DLL and resolve each entry.
    auto *Thunk =
        reinterpret_cast<PIMAGE_THUNK_DATA>(BaseBytes + ImportDesc->FirstThunk);

    for (; Thunk->u1.Function; Thunk++) {
      FARPROC FuncPtr = nullptr;

      // Determine whether this is an ordinal or named import.
#ifdef _WIN64
      bool IsOrdinal = (Thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64) != 0;
      size_t Ordinal = IMAGE_ORDINAL64(Thunk->u1.Ordinal);
#else
      bool IsOrdinal = (Thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG32) != 0;
      size_t Ordinal = IMAGE_ORDINAL32(Thunk->u1.Ordinal);
#endif

      if (IsOrdinal) {
        FuncPtr = GetProcAddress(ImportDLL, reinterpret_cast<LPCSTR>(Ordinal));
      } else {
        // Named import: the thunk RVA points to an IMAGE_IMPORT_BY_NAME
        // struct whose first two bytes are a hint, followed by the name.
        auto *ImportByName =
            reinterpret_cast<const char *>(BaseBytes + Thunk->u1.Function);
        const char *FuncName = ImportByName + 2; // Skip the 2-byte hint.
        if (!FuncName)
          break;
        FuncPtr = GetProcAddress(ImportDLL, FuncName);
      }

      if (!FuncPtr)
        continue;

      // Overwrite the IAT slot with the resolved function address.
      if (auto EC = PatchProcessBytes(
              reinterpret_cast<void *>(Thunk),
              llvm::StringRef(reinterpret_cast<const char *>(&FuncPtr),
                              sizeof(FuncPtr))))
        return EC;
    }
  }
  return std::error_code();
}

/// Locate ntdll!LdrpHandleTlsData by pattern-scanning ntdll.dll's image.
///
/// When an EXE is loaded via LoadLibraryW (rather than as the process's main
/// image), the Windows loader skips TLS directory processing.  We must call
/// LdrpHandleTlsData ourselves to register the module's TLS.  This function
/// cannot be reimplemented externally because it performs internal bookkeeping
/// that is tightly coupled to the NT loader's private data structures:
///   - It allocates a TLS index from PEB->TlsBitmap.
///   - It grows every thread's ThreadLocalStoragePointer array (the void**
///     indexed by TLS index) using the NT heap (RtlAllocateHeap /
///     RtlReAllocateHeap on the process heap).  This array is owned and
///     managed by the loader; reallocating it from outside ntdll would
///     corrupt the loader's bookkeeping and lead to crashes on subsequent
///     DLL loads or thread creation.
///   - It initializes TLS data from the PE template and registers the
///     module so that future threads and DLL loads preserve the entry.
/// Because the function is not exported, we locate it at runtime by
/// pattern-scanning ntdll.dll for the debug string "LdrpHandleTlsData"
/// (used for error reporting) and tracing back through the PE exception
/// handling structures.
///
/// x64 algorithm:
///   1. Find the string "LdrpHandleTlsData" in ntdll's mapped image.
///   2. Find a `lea rdx, [rip+disp]` (0x48 0x8D 0x15) that references it.
///      This instruction is in the SEH exception filter for the function.
///   3. Scan backwards for `push rbp` (0x40 0x55) — the filter's prologue.
///      Convert to RVA.
///   4. Iterate .pdata (IMAGE_DIRECTORY_ENTRY_EXCEPTION).  For each
///      RUNTIME_FUNCTION with UNW_FLAG_EHANDLER, decode the UNWIND_INFO's
///      C_SCOPE_TABLE and check if any scope record's Handler matches the
///      filter RVA.  The matching RUNTIME_FUNCTION.BeginAddress is the RVA
///      of LdrpHandleTlsData.
///
/// 32-bit algorithm:
///   1. Find the string, then find `push <string_addr>` in the filter.
///   2. Scan backwards for the filter's prologue pattern.
///   3. Find the EH4_SCOPETABLE referencing the filter address.
///   4. Find `push <scopetable_addr>` — that's in LdrpHandleTlsData's body.
///
/// ARM64 algorithm:
///   1. Find the string "LdrpHandleTlsData" in ntdll's mapped image.
///   2. Find an `ADRP Xd, #page` + `ADD Xd, Xd, #offset` pair that
///      references the string address.  This pair is in the SEH exception
///      filter for the function.
///   3. Scan backwards for the filter's prologue: `STP x29, x30, [sp, ...]`
///      (saving the frame pointer and link register).  Convert to RVA.
///   4. Iterate .pdata / .xdata.  For each function with exception data
///      (X bit set in the .xdata header), decode the C_SCOPE_TABLE and
///      check if any scope record's Handler matches the filter RVA.  The
///      matching .pdata BeginAddress is the RVA of LdrpHandleTlsData.
///
/// Based on the approach described at:
///   https://github.com/howmp/LdrpHandleTlsData
///
/// \return Function pointer to LdrpHandleTlsData, or nullptr if not found.
static void *FindLdrpHandleTlsData() {
  HMODULE Ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (!Ntdll)
    return nullptr;

  auto *Base = reinterpret_cast<uint8_t *>(Ntdll);
  auto *DosHdr = reinterpret_cast<IMAGE_DOS_HEADER *>(Base);
  auto *NtHdr = reinterpret_cast<IMAGE_NT_HEADERS *>(Base + DosHdr->e_lfanew);

  // Scan from the code section to the end of the mapped image.
  DWORD CodeStart = NtHdr->OptionalHeader.BaseOfCode;
  DWORD ImageSize = NtHdr->OptionalHeader.SizeOfImage;
  if (CodeStart >= ImageSize)
    return nullptr;

  uint8_t *Memory = Base + CodeStart;
  size_t MemSize = ImageSize - CodeStart;

  // Step 1: Find the string "LdrpHandleTlsData" in ntdll's image.
  static constexpr char Needle[] = "LdrpHandleTlsData";
  auto *StrLoc = std::search(Memory, Memory + MemSize, std::begin(Needle),
                             std::end(Needle) - 1);
  if (StrLoc == Memory + MemSize)
    return nullptr;
  auto StrAddr = reinterpret_cast<uintptr_t>(StrLoc);

#if defined(_M_X64) || defined(__x86_64__)
  // --- x64 path ---

  // Step 2: Find `lea rdx, [rip+disp32]` (48 8D 15 xx xx xx xx) referencing
  //         the string.  The displacement is a signed 32-bit offset from the
  //         instruction's end (IP + 7).
  constexpr uint8_t LeaRdxPrefix[] = {0x48, 0x8D, 0x15};
  uint8_t *Pos = Memory;
  uint8_t *End = Memory + MemSize;
  while (true) {
    Pos =
        std::search(Pos, End, std::begin(LeaRdxPrefix), std::end(LeaRdxPrefix));
    if (Pos == End)
      return nullptr;

    // Decode the RIP-relative displacement: target = IP + 7 + disp32.
    int32_t Disp;
    memcpy(&Disp, Pos + 3, sizeof(Disp));
    auto Target = reinterpret_cast<uintptr_t>(Pos + 7) + Disp;
    if (Target == StrAddr)
      break;
    Pos += 7;
  }

  // Step 3: Scan backwards for the exception filter's prologue: `push rbp`
  //         (0x40 0x55).
  uint8_t *FilterStart = nullptr;
  for (uint8_t *P = Pos - 1; P >= Memory; --P) {
    if (P[0] == 0x40 && P + 1 < End && P[1] == 0x55) {
      FilterStart = P;
      break;
    }
  }
  if (!FilterStart)
    return nullptr;

  // Convert to RVA (relative to ntdll's actual load address, not the
  // preferred ImageBase from the PE header, which may differ due to ASLR).
  uint32_t FilterRVA = static_cast<uint32_t>(FilterStart - Base);

  // Step 4: Search the exception directory (.pdata) for a RUNTIME_FUNCTION
  //         whose UNWIND_INFO scope table references our filter RVA.
  //
  //         Each RUNTIME_FUNCTION: { BeginAddress, EndAddress,
  //         UnwindInfoAddress } (all u32 RVAs).
  //
  //         UNWIND_INFO layout (packed bytes):
  //           [0]:  Version(3 bits) | Flags(5 bits)
  //           [1]:  SizeOfProlog
  //           [2]:  CountOfCodes
  //           [3]:  FrameRegister(4 bits) | FrameOffset(4 bits)
  //           [4..]: UNWIND_CODE[CountOfCodes]  (2 bytes each)
  //           (padding to 4-byte boundary if CountOfCodes is odd)
  //           ExceptionHandler RVA (4 bytes, present if UNW_FLAG_EHANDLER)
  //           C_SCOPE_TABLE: Count(u32), { Begin, End, Handler, Target }[]
  //
  //         We look for a scope table entry where Handler == FilterRVA.
  //         The enclosing RUNTIME_FUNCTION.BeginAddress is the RVA of
  //         LdrpHandleTlsData.
  DWORD ExcDirSize = 0;
  auto *ExcData = GetImageDirectoryEntryData(
      Ntdll, IMAGE_DIRECTORY_ENTRY_EXCEPTION, &ExcDirSize);
  if (!ExcData)
    return nullptr;
  auto *RtFuncs = reinterpret_cast<IMAGE_RUNTIME_FUNCTION_ENTRY *>(ExcData);
  DWORD NumFunctions = ExcDirSize / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);

  for (DWORD I = 0; I < NumFunctions; ++I) {
    if (RtFuncs[I].UnwindInfoAddress == 0)
      continue;
    auto *UI = Base + RtFuncs[I].UnwindInfoAddress;

    // UNWIND_INFO.Flags is in bits 3-7 of the first byte.
    uint8_t Flags = (UI[0] >> 3) & 0x1F;
    // UNW_FLAG_EHANDLER = 1: function has an exception handler.
    // UNW_FLAG_CHAININFO = 4: skip chained entries.
    if (!(Flags & 0x01) || (Flags & 0x04))
      continue;

    uint8_t CntCodes = UI[2];
    // Align the unwind codes array to a 4-byte boundary (pad if odd count).
    DWORD CodesBytes = CntCodes * 2;
    if (CodesBytes & 2)
      CodesBytes += 2; // Round up to next 4-byte boundary.

    // Handler data starts after the 4-byte header + aligned unwind codes.
    auto *HandlerData = reinterpret_cast<uint32_t *>(UI + 4 + CodesBytes);
    // HandlerData[0] = exception handler RVA (__C_specific_handler).
    // HandlerData[1] = C_SCOPE_TABLE.Count.
    uint32_t Count = HandlerData[1];
    if (Count == 0 || Count > 64) // Sanity check.
      continue;

    // Each scope record: { BeginAddress, EndAddress, HandlerAddress,
    //                       JumpTarget } — 4 DWORDs.
    for (uint32_t J = 0; J < Count; ++J) {
      uint32_t Handler = HandlerData[2 + J * 4 + 2];
      if (Handler == FilterRVA)
        return static_cast<void *>(Base + RtFuncs[I].BeginAddress);
    }
  }
  return nullptr; // Filter not found in any RUNTIME_FUNCTION scope table.

#elif defined(_M_IX86) || defined(__i386__)
  // --- 32-bit path ---

  // Step 2: Find a reference to the string address (a `push <imm32>`
  //         instruction in the exception filter).
  auto StrAddrLE = static_cast<uint32_t>(StrAddr);
  auto *StrRefLoc = std::search(Memory, Memory + MemSize,
                                reinterpret_cast<uint8_t *>(&StrAddrLE),
                                reinterpret_cast<uint8_t *>(&StrAddrLE) + 4);
  if (StrRefLoc == Memory + MemSize)
    return nullptr;

  // Step 3: Scan backwards for the exception filter's prologue.
  //         Pattern: mov eax, [ebp+0xEC] → bytes ending with 0xEC, then
  //         next instruction starts with 0x8B.
  uint8_t *FilterFunc = nullptr;
  constexpr uint8_t PrologKey[] = {0xEC, 0x8B};
  for (uint8_t *P = StrRefLoc - 1; P >= Memory + 2; --P) {
    if (P[0] == PrologKey[0] && P[1] == PrologKey[1]) {
      FilterFunc = P - 2; // Back up past `mov eax, [ebp+...]`.
      break;
    }
  }
  if (!FilterFunc)
    return nullptr;

  auto FilterAddr =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(FilterFunc));

  // Step 4: Find the EH4_SCOPETABLE that references this filter function.
  //         Layout: { GSCookieOffset(u32), GSCookieXOROffset(u32),
  //                   EHCookieOffset(u32), EHCookieXOROffset(u32),
  //                   { EnclosingLevel(u32), FilterFunc(u32),
  //                     HandlerAddress(u32) }[...] }
  //         We search for { EHCookieXOROffset=0, EnclosingLevel=0xFFFFFFFE,
  //                          FilterFunc=FilterAddr }.
  uint8_t ScopeKey[12];
  uint32_t Zero = 0, EncLevel = 0xFFFFFFFE;
  memcpy(ScopeKey, &Zero, 4);
  memcpy(ScopeKey + 4, &EncLevel, 4);
  memcpy(ScopeKey + 8, &FilterAddr, 4);

  auto *ScopeMatch = std::search(Memory, Memory + MemSize, std::begin(ScopeKey),
                                 std::end(ScopeKey));
  if (ScopeMatch == Memory + MemSize)
    return nullptr;

  // The match is at the EHCookieXOROffset field; the scopetable starts
  // 12 bytes earlier (GSCookieOffset).
  auto ScopeTableAddr =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ScopeMatch) - 12);

  // Step 5: Find `push <scopetable_addr>` in LdrpHandleTlsData's body.
  //         The function starts a few bytes before the push.
  auto *FnRef = std::search(Memory, Memory + MemSize,
                            reinterpret_cast<uint8_t *>(&ScopeTableAddr),
                            reinterpret_cast<uint8_t *>(&ScopeTableAddr) + 4);
  if (FnRef == Memory + MemSize)
    return nullptr;

  // The push instruction is 5 bytes (0x68 <imm32>), preceded by another
  // push (0x6A <imm8> = 2 bytes).  The function starts 3 bytes before
  // the scopetable reference.
  return reinterpret_cast<void *>(FnRef - 3);

#elif defined(_M_ARM64) || defined(__aarch64__)
  // --- ARM64 path ---
  //
  // ARM64 uses ADRP+ADD instruction pairs instead of x64's RIP-relative LEA
  // to form PC-relative addresses.  The algorithm mirrors the x64 path but
  // decodes ARM64 instructions:
  //
  //   ADRP Xd, #page    — loads the 4KB-aligned page address into Xd
  //     Encoding: 1 | immlo[1:0] | 10000 | immhi[18:0] | Rd[4:0]
  //     The 21-bit immediate (immhi:immlo) is shifted left by 12 and added
  //     to the instruction's page-aligned PC (PC & ~0xFFF).
  //
  //   ADD  Xd, Xn, #off  — adds the 12-bit page offset
  //     Encoding: 1 | 0010001 | sh | imm12[11:0] | Rn[4:0] | Rd[4:0]
  //     With sh=0, the immediate is used directly (0–4095).
  //
  // We scan for an ADRP+ADD pair whose combined target equals the address
  // of the "LdrpHandleTlsData" string.  The ADRP must be followed by an
  // ADD that uses the same destination register as both source and dest.
  //
  // ARM64 .pdata uses the same RUNTIME_FUNCTION layout as x64, and the
  // .xdata exception handler + C_SCOPE_TABLE layout is structurally
  // identical (Exception Handler RVA followed by scope table with Count
  // and {Begin, End, Handler, Target} entries).

  // Helper: decode an ADRP instruction.
  //   On success, sets *PageAddr to the target page address and *Rd to the
  //   destination register number.  Returns true on success.
  auto DecodeADRP = [](const uint8_t *InsnPtr, uintptr_t *PageAddr,
                       uint32_t *Rd) -> bool {
    uint32_t Insn;
    memcpy(&Insn, InsnPtr, 4);
    // Check opcode: bit 31 = 1, bits 28-24 = 10000
    if ((Insn & 0x9F000000u) != 0x90000000u)
      return false;
    *Rd = Insn & 0x1Fu;
    // immhi = bits 23:5, immlo = bits 30:29
    uint32_t ImmHi = (Insn >> 5) & 0x7FFFFu; // 19 bits
    uint32_t ImmLo = (Insn >> 29) & 0x3u;    // 2 bits
    // Combine into a 21-bit signed immediate, then shift left by 12.
    int64_t Imm = static_cast<int64_t>((ImmHi << 2) | ImmLo);
    // Sign-extend from 21 bits.
    if (Imm & (1 << 20))
      Imm |= ~((int64_t(1) << 21) - 1);
    Imm <<= 12;
    // ADRP target = (PC & ~0xFFF) + Imm
    uintptr_t PC = reinterpret_cast<uintptr_t>(InsnPtr);
    *PageAddr = (PC & ~uintptr_t(0xFFF)) + static_cast<uintptr_t>(Imm);
    return true;
  };

  // Helper: decode an ADD immediate instruction.
  //   On success, sets *Imm to the immediate value.  Returns true only if
  //   the instruction is a 64-bit ADD immediate with Rd == Rn == ExpectedReg
  //   (i.e. the register matches the preceding ADRP's destination).
  auto DecodeADDImm = [](const uint8_t *InsnPtr, uint32_t ExpectedReg,
                         int64_t *Imm) -> bool {
    uint32_t Insn;
    memcpy(&Insn, InsnPtr, 4);
    // Check opcode: sf=1 (64-bit), op=0 (ADD), S=0, bits 28:23 = 010001
    // Full mask: bit 31 = 1, bits 30:23 = 00100010 => 0xFF800000 == 0x91000000
    if ((Insn & 0xFF800000u) != 0x91000000u)
      return false;
    uint32_t Rd = Insn & 0x1Fu;
    uint32_t Rn = (Insn >> 5) & 0x1Fu;
    // Both Rd and Rn must match the ADRP's destination register.
    if (Rd != ExpectedReg || Rn != ExpectedReg)
      return false;
    uint32_t Sh = (Insn >> 22) & 0x1u;
    uint32_t Imm12 = (Insn >> 10) & 0xFFFu;
    *Imm = Sh ? (int64_t(Imm12) << 12) : int64_t(Imm12);
    return true;
  };

  // Step 2: Find an ADRP+ADD pair that references the string address.
  //         ARM64 instructions are always 4-byte aligned.
  uint8_t *Pos = Memory;
  uint8_t *End = Memory + MemSize;
  // Align Pos to 4-byte boundary.
  Pos = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(Pos) + 3) &
                                    ~uintptr_t(3));

  uint8_t *FilterInsn = nullptr;
  for (; Pos + 8 <= End; Pos += 4) {
    uintptr_t PageAddr;
    uint32_t AdrpRd;
    if (!DecodeADRP(Pos, &PageAddr, &AdrpRd))
      continue;
    // The ADD must immediately follow the ADRP and use the same register.
    int64_t AddImm;
    if (!DecodeADDImm(Pos + 4, AdrpRd, &AddImm))
      continue;
    uintptr_t Target = PageAddr + static_cast<uintptr_t>(AddImm);
    if (Target == StrAddr) {
      FilterInsn = Pos;
      break;
    }
  }
  if (!FilterInsn)
    return nullptr;

  // Step 3: Scan backwards for the exception filter's prologue.
  //         On ARM64, look for a typical prologue: STP x29, x30, [sp, #-N]!
  //         Encoding: 101 0100 110 imm7 11110 11101 (x30=LR, x29=FP)
  //         Mask for STP x29, x30, [sp, #imm]! (pre-indexed):
  //           bits 31:22 = 1010100110, Rt2=x30 (bits 14:10=11110),
  //           Rn=sp (bits 9:5=11111), Rt=x29 (bits 4:0=11101)
  //         We match: (Insn & 0xFFC003FF) == 0xA98003FD
  //         But also accept STP with positive offset (post-indexed or signed):
  //         Simpler approach: scan for any STP that saves x29,x30.
  uint8_t *FilterStart = nullptr;
  for (uint8_t *P = FilterInsn - 4; P >= Memory; P -= 4) {
    uint32_t Insn;
    memcpy(&Insn, P, 4);
    // Match STP x29, x30, [sp, #imm]! (pre-indexed, writeback)
    // Encoding: 1010 1001 10xx xxxx 0111 1011 1011 1101
    //   bits 31:22 = 1010100110 (STP pre-indexed, 64-bit)
    //   Rt2 (bits 14:10) = 11110 (x30)
    //   Rn  (bits 9:5)   = 11111 (sp)
    //   Rt  (bits 4:0)   = 11101 (x29)
    if ((Insn & 0xFFC07FFFu) == 0xA9807BFDu) {
      FilterStart = P;
      break;
    }
    // Also match: STP x29, x30, [sp, #imm] (signed offset, no writeback)
    // bits 31:22 = 1010 1001 01
    if ((Insn & 0xFFC07FFFu) == 0xA9007BFDu) {
      FilterStart = P;
      break;
    }
  }
  if (!FilterStart)
    return nullptr;

  uint32_t FilterRVA = static_cast<uint32_t>(FilterStart - Base);

  // Step 4: Search the exception directory (.pdata) for a RUNTIME_FUNCTION
  //         whose .xdata scope table references our filter RVA.
  //
  //         ARM64 .pdata records are 8 bytes each (same RUNTIME_FUNCTION as
  //         x64): { BeginAddress(u32), UnwindData(u32) }.
  //         If UnwindData bit 0 is set, it's packed unwind data (no .xdata);
  //         otherwise bits 1:0 are 0 and it's an RVA to .xdata.
  //
  //         .xdata layout (see MS docs "ARM64 exception handling"):
  //           Word 0: FunctionLength(18) | Vers(2) | X(1) | E(1) |
  //                   EpilogCount(5) | CodeWords(5)
  //           (optional extension word if EpilogCount==0 && CodeWords==0)
  //           Epilog scopes (EpilogCount words, unless E==1)
  //           Unwind codes (CodeWords words)
  //           If X==1: Exception Handler RVA (u32)
  //                    C_SCOPE_TABLE: Count(u32), {Begin,End,Handler,Target}[]
  DWORD ExcDirSize = 0;
  auto *ExcData = GetImageDirectoryEntryData(
      Ntdll, IMAGE_DIRECTORY_ENTRY_EXCEPTION, &ExcDirSize);
  if (!ExcData)
    return nullptr;

  // ARM64 .pdata entries are 8 bytes (two DWORDs).
  auto *PdataEntries = reinterpret_cast<uint32_t *>(ExcData);
  DWORD NumEntries = ExcDirSize / 8;

  for (DWORD I = 0; I < NumEntries; ++I) {
    uint32_t BeginAddr = PdataEntries[I * 2];
    uint32_t UnwindData = PdataEntries[I * 2 + 1];

    // Skip packed unwind data (bit 0 set) — no .xdata record.
    if (UnwindData & 0x3)
      continue;

    auto *XData = reinterpret_cast<uint32_t *>(Base + UnwindData);
    uint32_t Header = XData[0];

    // X bit (bit 20): exception data present?
    bool HasExceptionData = (Header >> 20) & 1;
    if (!HasExceptionData)
      continue;

    // E bit (bit 21): single epilog packed in header?
    bool SingleEpilog = (Header >> 21) & 1;
    uint32_t EpilogCount = (Header >> 22) & 0x1F;
    uint32_t CodeWords = (Header >> 27) & 0x1F;

    uint32_t WordIdx = 1; // Start after header word.

    // Extension word present if both EpilogCount and CodeWords are 0.
    if (EpilogCount == 0 && CodeWords == 0) {
      if (WordIdx >= ExcDirSize / 4)
        continue;
      uint32_t ExtWord = XData[WordIdx++];
      EpilogCount = ExtWord & 0xFFFF;
      CodeWords = (ExtWord >> 16) & 0xFF;
    }

    // Skip epilog scope words (unless single epilog packed in header).
    if (!SingleEpilog)
      WordIdx += EpilogCount;

    // Skip unwind code words.
    WordIdx += CodeWords;

    // Now at the exception handler RVA, followed by C_SCOPE_TABLE.
    // XData[WordIdx]   = Exception Handler RVA (__C_specific_handler)
    // XData[WordIdx+1] = C_SCOPE_TABLE.Count
    // XData[WordIdx+2..] = scope entries {Begin, End, Handler, Target}
    uint32_t Count = XData[WordIdx + 1];
    if (Count == 0 || Count > 64) // Sanity check.
      continue;

    for (uint32_t J = 0; J < Count; ++J) {
      uint32_t Handler = XData[WordIdx + 2 + J * 4 + 2];
      if (Handler == FilterRVA)
        return static_cast<void *>(Base + BeginAddr);
    }
  }
  return nullptr; // Filter not found in any .pdata/.xdata scope table.

#else
  return nullptr; // Unsupported architecture.
#endif
}

/// Register a loaded PE module's TLS directory with the NT loader.
///
/// When an EXE is loaded via LoadLibraryW (as opposed to being the process's
/// main image or a DLL), the Windows loader does not process its TLS directory.
/// This function calls the internal ntdll function LdrpHandleTlsData to
/// properly register the module's TLS, so that:
///   - A TLS index is allocated from PEB->TlsBitmap.
///   - ThreadLocalStoragePointer is grown on all existing threads.
///   - TLS data is initialized from the PE template.
///   - Future threads automatically get the correct TLS state.
///   - Subsequent DLL loads that grow the TLS array preserve our entry.
///
/// LdrpHandleTlsData is found at runtime via pattern scanning of ntdll.dll
/// (see FindLdrpHandleTlsData).  It takes a single parameter: a pointer to
/// the module's LDR_DATA_TABLE_ENTRY.  We obtain this from the PEB's
/// InLoadOrderModuleList.
///
/// \param Mod  ExecModule whose TLS should be initialized.
/// \return std::error_code on failure, default-constructed on success.
static std::error_code InitializeStaticTLS(ExecModule &Mod) {
  HMODULE ModuleBase = reinterpret_cast<HMODULE>(Mod.Handle);

  // Check if the module has a TLS directory at all.
  DWORD TlsDirSize = 0;
  auto *TlsDir = GetImageDirectoryEntryData(
      ModuleBase, IMAGE_DIRECTORY_ENTRY_TLS, &TlsDirSize);
  if (!TlsDir)
    return std::error_code(); // No TLS directory — nothing to do.

  // Resolve LdrpHandleTlsData from ntdll.dll via pattern scanning.
  // This avoids downloading ntdll.pdb from Microsoft Symbol Servers.
  using LdrpHandleTlsDataFn = NTSTATUS(NTAPI *)(PVOID /*LDR_DATA_TABLE_ENTRY*/);
  static auto *LdrpHandleTlsData =
      reinterpret_cast<LdrpHandleTlsDataFn>(FindLdrpHandleTlsData());
  if (!LdrpHandleTlsData)
    return make_error_code(std::errc::function_not_supported);

  // Walk the PEB's InLoadOrderModuleList to find the LDR_DATA_TABLE_ENTRY
  // for our module.  Each entry's DllBase corresponds to the HMODULE.
  PPEB Peb = NtCurrentTeb()->ProcessEnvironmentBlock;
  auto *Ldr = Peb->Ldr;
  auto *Head = &Ldr->InMemoryOrderModuleList;

  PVOID LdrEntry = nullptr;
  for (auto *Entry = Head->Flink; Entry != Head; Entry = Entry->Flink) {
    // InMemoryOrderModuleList links are at offset +0x10 (x64) or +0x08 (x86)
    // within LDR_DATA_TABLE_ENTRY.  The CONTAINING_RECORD macro adjusts back
    // to the start of the structure.
    auto *DataEntry =
        CONTAINING_RECORD(Entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
    if (DataEntry->DllBase == static_cast<PVOID>(ModuleBase)) {
      // LdrpHandleTlsData expects a pointer to the full internal
      // LDR_DATA_TABLE_ENTRY, which starts at InLoadOrderLinks
      // (the very beginning of the structure).  Since the SDK's struct
      // starts at InLoadOrderLinks, DataEntry is what we need.
      LdrEntry = static_cast<PVOID>(DataEntry);
      break;
    }
  }

  if (!LdrEntry)
    return make_error_code(std::errc::no_such_process);

  NTSTATUS Status = LdrpHandleTlsData(LdrEntry);
  if (Status != 0)
    return make_error_code(std::errc::operation_not_permitted);

  return std::error_code();
}

/// Snapshot the current thread's static TLS data for the given module.
///
/// Returns a heap-allocated copy of the module's TLS slot contents, which
/// reflects post-CRT-init state.  This snapshot is later passed to ResetTLS()
/// before each tool invocation so that every call starts from a clean TLS
/// state.
static void *SaveTLS(HANDLE H) {
  DWORD Size = 0;
  auto *TlsDir = reinterpret_cast<PIMAGE_TLS_DIRECTORY>(
      GetImageDirectoryEntryData(H, IMAGE_DIRECTORY_ENTRY_TLS, &Size));
  if (!TlsDir)
    return nullptr;

  size_t Len = TlsDir->EndAddressOfRawData - TlsDir->StartAddressOfRawData;
  void *Buf = llvm::safe_malloc(Len);

  // Access the TEB's ThreadLocalStoragePointer, which is an array of void*
  // indexed by TLS index.
  //
  // The Windows SDK's TEB structure exposes this field as Reserved1[11].
  // In the actual NT TEB layout (from ntdll PDB symbols / winternl.h), this
  // corresponds to the ThreadLocalStoragePointer field:
  //
  //   Offset  Field
  //   ------  -----
  //   x64:  0x0058  ThreadLocalStoragePointer  (Reserved1[11] at 8*11=0x58)
  //   x86:  0x002C  ThreadLocalStoragePointer  (Reserved1[11] at 4*11=0x2C)
  //   ARM64: 0x0058  ThreadLocalStoragePointer  (same as x64)
  //
  // This offset has been stable across all Windows versions from XP through
  // Windows 11 (and Server equivalents).  It is also used by the NT loader
  // itself, the CRT, and debuggers.  See:
  //   - https://www.vergiliusproject.com/kernels/x64/windows-11/24H2/_TEB
  //   - Windows SDK <winternl.h> (declares Reserved1[12] covering offsets
  //     0x00..0x5F on x64)
  PTEB Teb = NtCurrentTeb();
  auto **TlsArray = reinterpret_cast<uint8_t **>(Teb->Reserved1[11]);

  ULONG TlsIndex = *reinterpret_cast<ULONG *>(TlsDir->AddressOfIndex);

  // Copy from the live TLS storage (not the PE template), so we capture the
  // state after CRT initialization.
  memcpy(Buf, TlsArray[TlsIndex], Len);

  return Buf;
}

/// Reset the current thread's static TLS data for the given module.
///
/// If \p Buffer is non-null, copies from that saved snapshot (produced by
/// SaveTLS).  Otherwise, copies from the original PE .tls template.  This
/// ensures each tool invocation starts with deterministic TLS state.
static void ResetTLS(HANDLE H, void *Buffer = nullptr) {
  DWORD Size = 0;
  auto *TlsDir = reinterpret_cast<PIMAGE_TLS_DIRECTORY>(
      GetImageDirectoryEntryData(H, IMAGE_DIRECTORY_ENTRY_TLS, &Size));
  if (!TlsDir)
    return;

  // See SaveTLS() for documentation on TEB->Reserved1[11].
  PTEB Teb = NtCurrentTeb();
  auto **TlsArray = reinterpret_cast<uint8_t **>(Teb->Reserved1[11]);

  size_t Len = TlsDir->EndAddressOfRawData - TlsDir->StartAddressOfRawData;
  ULONG TlsIndex = *reinterpret_cast<ULONG *>(TlsDir->AddressOfIndex);
  const void *SrcBuf =
      Buffer ? Buffer
             : reinterpret_cast<const void *>(TlsDir->StartAddressOfRawData);

  memcpy(TlsArray[TlsIndex], SrcBuf, Len);
}

/// Initialize the CRT for a loaded EXE module on a dedicated helper thread.
///
/// The CRT startup (mainCRTStartup / __scrt_common_main) must run on a thread
/// whose TLS state belongs to the loaded module.  Since InitializeStaticTLS()
/// called LdrpHandleTlsData to register the module's TLS with the NT loader,
/// new threads automatically get a properly sized ThreadLocalStoragePointer
/// array that includes the module's TLS slot.
///
/// This function spawns a helper thread that:
///   1. Calls the module's CRT init routine (signals \p InitDoneEvent when
///      ready).
///   2. Calls the module's CRT main entry point.
///   3. Waits for \p ExitEvent before returning, so static destructors do not
///      run prematurely.
///
/// After CRT init completes, the helper thread is suspended and kept alive
/// for the lifetime of the process.  At shutdown, TerminateAll() resumes it
/// and signals the exit event.
static std::error_code InitializeCRT(ExecModule &Mod) {
  std::string ModuleName = Mod.Name.str();

  auto CRTMain = Mod.ToolSymTab.MainCRT;
  auto InitCRT = Mod.ToolSymTab.CRTInit;

  // InitDoneEvent: signaled by the CRT init routine once initialization is
  //   complete, unblocking the calling thread.
  // ExitEvent: signaled at process shutdown to let the helper thread
  //   terminate (running static destructors).
  HANDLE InitDoneEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  HANDLE ExitEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

  std::thread T([&]() {
    llvm::CrashRecoveryContext CRC;
    CRC.RunSafely([&] {
      InitCRT(InitDoneEvent, ExitEvent);
      CRTMain(Mod.Handle);
    });
    // If RunSafely returned without signaling (e.g. due to a crash),
    // signal InitDoneEvent so the main thread does not deadlock.
    SetEvent(InitDoneEvent);
  });

  // Block until the module's CRT init is done.
  WaitForSingleObject(InitDoneEvent, INFINITE);
  CloseHandle(InitDoneEvent);

  // Label the thread for debugger visibility, then suspend it.
  llvm::SmallVector<wchar_t, MAX_PATH> WThreadName;
  std::string ThreadName = "CRT Init for " + ModuleName;
  llvm::sys::windows::UTF8ToUTF16(ThreadName.c_str(), WThreadName);
  SetThreadDescription(T.native_handle(), WThreadName.data());
  SuspendThread(T.native_handle());

  Mod.ExitEvent = ExitEvent;
  Mod.TLSThreadHandle = T.native_handle();
  T.detach();
  return std::error_code();
}

/// Locate the module's tool entry point via its export table.
///
/// Resolves the "getCallableTool" symbol exported by the loaded EXE (via
/// extern "C" LLVM_ALWAYS_EXPORT in LLVMDriver.h) and calls it to populate
/// the module's ToolSymbolTable.  The exported function uses an output
/// parameter (void*) to avoid returning a C++ type across extern "C"
/// boundaries.
///
/// \param Mod  Module whose entry points should be resolved.
/// \return std::error_code on failure, default-constructed on success.
std::error_code FindEntryPoints(ExecModule &Mod) {
  using GetCallableToolFn = void (*)(void *);
  auto *FuncPtr = ::GetProcAddress(reinterpret_cast<HMODULE>(Mod.Handle),
                                   "getCallableTool");
  if (!FuncPtr)
    return mapWindowsError(GetLastError());
  reinterpret_cast<GetCallableToolFn>(FuncPtr)(&Mod.ToolSymTab);

  return std::error_code();
}

/// Invoke a loaded module's tool entry point.
///
/// Resets the module's TLS data to the post-CRT-init snapshot, fires
/// the DLL_THREAD_ATTACH TLS callback (so CRT per-invocation state is
/// reinitialized), then calls the tool's main function.
int Launch(int ModuleIndex, const ToolContext &Ctx,
           ArrayRef<const char *> Args) {
  ExecModule &Mod = ExecModule::getModule(ModuleIndex);

  ResetTLS(Mod.Handle, Mod.TLSInitBuffer);

  Mod.ToolSymTab.TLSInit(nullptr, DLL_THREAD_ATTACH, nullptr);

  return Mod.ToolSymTab.Tool.call(Ctx, Args);
}

/// Perform post-invocation cleanup for a loaded module.
///
/// Fires the DLL_THREAD_DETACH TLS callback so that per-invocation CRT
/// state is torn down.
void Terminate(int ModuleIndex, std::string &StdErr, std::string &StdOut) {
  ExecModule &Mod = ExecModule::getModule(ModuleIndex);

  Mod.ToolSymTab.TLSDtor(nullptr, DLL_THREAD_DETACH, nullptr);
}

/// Shut down all loaded modules and release resources.
///
/// For each module:
///   1. Resumes the suspended CRT helper thread and signals its exit event
///      so that static destructors can run.
///   2. Waits for the helper thread to finish.
///   3. Frees the TLS snapshot buffer (TLSInitBuffer).
///   4. Closes the ExitEvent handle.
///   5. Unloads the module (FreeLibrary).
void TerminateAll() {
  for (auto &Mod : LoadedModules) {
    // Let the CRT helper thread run its static destructors and exit.
    ResumeThread(Mod.TLSThreadHandle);
    SetEvent(Mod.ExitEvent);
    WaitForSingleObject(Mod.TLSThreadHandle, INFINITE);

    // Free the TLS snapshot taken after CRT init.
    free(Mod.TLSInitBuffer);
    Mod.TLSInitBuffer = nullptr;

    // Close the exit event handle.
    if (Mod.ExitEvent) {
      CloseHandle(Mod.ExitEvent);
      Mod.ExitEvent = nullptr;
    }

    // Unload the module.
    if (Mod.Handle) {
      FreeLibrary(reinterpret_cast<HMODULE>(Mod.Handle));
      Mod.Handle = nullptr;
    }
  }
  LoadedModules.clear();
}

/// Load a tool EXE in-process and prepare it for repeated invocation.
///
/// Given a module name (e.g. "lld-link"), locates the corresponding .exe in
/// the same directory as the running binary, loads it via LoadLibraryW, and
/// performs the full initialization sequence:
///   1. IAT patching        (RemapIAT)
///   2. Entry point lookup  (FindEntryPoints)
///   3. TLS registration    (InitializeStaticTLS / LdrpHandleTlsData)
///   4. CRT init            (InitializeCRT)
///   5. TLS snapshot        (SaveTLS)
///
/// On success, the tool is registered in the driver's tool table under both
/// its stem name (e.g. "lld-link") and its canonical name (e.g. "lld"), and
/// a CallableTool handle is returned.
ErrorOr<llvm::ToolMainFn>
loadCallableModule(StringRef ModuleName, std::vector<StringRef> &ToolNames) {
  // Determine the directory of the currently running binary.
  wchar_t CurrentModW[MAX_PATH];
  DWORD Length = ::GetModuleFileNameW(nullptr, CurrentModW, MAX_PATH);
  if (Length == 0 || Length == MAX_PATH)
    return mapWindowsError(GetLastError());

  SmallString<MAX_PATH> CurrentModUTF8;
  if (std::error_code EC =
          windows::UTF16ToUTF8(CurrentModW, Length, CurrentModUTF8))
    return EC;

  std::string CurrentDir =
      llvm::sys::path::parent_path(CurrentModUTF8.str()).str();

  // Build the full path: <dir>/<module-stem>.exe
  SmallString<128> ModulePath(CurrentDir);
  llvm::sys::path::append(ModulePath, llvm::sys::path::filename(ModuleName));
  llvm::sys::path::replace_extension(ModulePath, ".exe");

  if (!llvm::sys::fs::exists(ModulePath))
    return make_error_code(std::errc::no_such_file_or_directory);

  SmallVector<wchar_t, MAX_PATH> ModulePathUTF16;
  if (std::error_code EC = windows::UTF8ToUTF16(ModulePath, ModulePathUTF16))
    return EC;

  HMODULE H = ::LoadLibraryW(ModulePathUTF16.data());
  if (!H)
    return mapWindowsError(GetLastError());

  // DLL mode is not yet implemented -- just load and return a stub handle.
  // FIXME: add and handle entry points when we build in DLL mode.
  if (llvm::sys::path::extension(ModulePath).equals_insensitive(".dll"))
    return make_error_code(std::errc::not_supported);

  // RAII guard: unloads the module if we bail out with an error.
  ScopedCommonHandle FreeMod(H);

  // Only EXE modules are supported for in-process loading.
  if (!llvm::sys::path::extension(ModulePath).equals_insensitive(".exe"))
    return make_error_code(std::errc::invalid_argument);

  // Verify the loaded PE matches the host process architecture.  A mismatch
  // (e.g. an x86 binary on an ARM64 host) would cause subtle failures later.
  {
    auto *Base = reinterpret_cast<const uint8_t *>(H);
    auto *DosHdr = reinterpret_cast<const IMAGE_DOS_HEADER *>(Base);
    auto *NtHdr =
        reinterpret_cast<const IMAGE_NT_HEADERS *>(Base + DosHdr->e_lfanew);
#if defined(_M_X64) || defined(__x86_64__)
    constexpr WORD ExpectedMachine = IMAGE_FILE_MACHINE_AMD64;
#elif defined(_M_IX86) || defined(__i386__)
    constexpr WORD ExpectedMachine = IMAGE_FILE_MACHINE_I386;
#elif defined(_M_ARM64) || defined(__aarch64__)
    constexpr WORD ExpectedMachine = IMAGE_FILE_MACHINE_ARM64;
#else
#error "Unsupported architecture"
#endif
    if (NtHdr->FileHeader.Machine != ExpectedMachine)
      return make_error_code(std::errc::executable_format_error);
  }

  ExecModule Mod;
  Mod.Name = Saver.save(llvm::sys::path::filename(ModulePath));
  Mod.FullPath = Saver.save(CurrentDir);
  Mod.Handle = H;

  // Step 1: Patch the IAT so the module's imports resolve correctly.
  if (auto EC = RemapIAT(H))
    return EC;

  // TODO: redirect rpmalloc calls in the module to the host process's
  // allocator instance if rpmalloc is enabled.  Currently, the loaded
  // module's rpmalloc lazily initializes itself on first allocation via
  // ENABLE_PRELOAD's get_thread_heap() fallback (which calls
  // rpmalloc_initialize()), so this is not strictly required.  However,
  // redirecting would avoid maintaining two independent rpmalloc heaps
  // in the same process.  PatchRPMalloc(H);

  // Step 2: Resolve the module's tool entry point.
  if (auto EC = FindEntryPoints(Mod))
    return EC;

  // Step 3: Register the module's TLS with the NT loader via LdrpHandleTlsData.
  if (auto EC = InitializeStaticTLS(Mod))
    return EC;

  // Step 4: Run the module's CRT startup on a helper thread.
  if (auto EC = InitializeCRT(Mod))
    return EC;

  // Step 5: Snapshot the post-CRT TLS state so it can be restored before
  // each tool invocation.
  Mod.TLSInitBuffer = SaveTLS(Mod.Handle);

  LoadedModules.push_back(Mod);
  unsigned ModIdx = LoadedModules.size();

  // Wrap Launch/Terminate into a callable lambda.
  auto Fn = [=](ArrayRef<const char *> Args, const ToolContext &Ctx) -> int {
    int Res = Launch(ModIdx, Ctx, Args);
    std::string StdErr, StdOut;
    Terminate(ModIdx, StdErr, StdOut);
    return Res;
  };

  // Register under the invoked name (e.g. "lld-link").
  ToolNames.push_back(llvm::sys::path::stem(Mod.Name));
  // Register under the canonical name provided by the binary (e.g. "lld").
  ToolNames.push_back(Mod.ToolSymTab.Tool.ToolName);

  // Everything succeeded -- release the RAII guard to keep the module loaded.
  FreeMod.take();

  return Fn;
}
