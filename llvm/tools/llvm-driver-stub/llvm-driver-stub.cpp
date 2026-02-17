//===-- llvm-driver-stub.cpp - Minimal stub for llvm-driver aliases
//--------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A minimal standalone executable (~3-4 KB) used as a fallback when hard links
// to llvm.exe cannot be created (e.g. FAT32/exFAT filesystems, cross-volume
// installs).
//
// When invoked as e.g. "clang-cl.exe", this stub:
//   1. Determines its own filename (e.g. "clang-cl.exe").
//   2. Locates "llvm.exe" in the same directory.
//   3. Spawns llvm.exe as a child process, passing the stub's filename as
//      argv[0] so that llvm.exe dispatches to the correct tool.
//   4. Forwards the child's exit code.
//
// The stub inherits the parent's console and standard handles, so it works
// correctly in pipelines and with I/O redirection.  Ctrl+C is also forwarded
// naturally since both processes share the same console.
//
// Build requirements:
//   - Windows-only, no LLVM libraries.
//   - Built with /NODEFAULTLIB and a custom entry point -- no CRT at all.
//   - Only imports from kernel32.dll (always pre-loaded by the OS).
//   - Resulting binary is ~3-4 KB with zero DLL load overhead at startup.
//
//===----------------------------------------------------------------------===//

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// -- Inline replacements for CRT string functions (avoid ucrt dependency) -----

static DWORD WStrLen(const wchar_t *S) {
  // Use a volatile pointer to prevent the compiler from recognizing this
  // loop as a wcslen pattern and emitting a call to wcslen, which does not
  // exist in a /NODEFAULTLIB build.
  const volatile wchar_t *P = S;
  DWORD Len = 0;
  while (*P++)
    ++Len;
  return Len;
}

static void WMemCpy(wchar_t *Dst, const wchar_t *Src, DWORD Count) {
  // Use a volatile pointer to prevent the compiler from recognizing this
  // loop as a memcpy pattern and emitting a call to memcpy, which does not
  // exist in a /NODEFAULTLIB build.
  volatile wchar_t *D = Dst;
  for (DWORD I = 0; I < Count; ++I)
    D[I] = Src[I];
}

/// Find the last path separator ('\\' or '/') in a wide string.
/// Returns a pointer to it, or nullptr if not found.
static wchar_t *FindLastSep(wchar_t *Path) {
  wchar_t *Last = nullptr;
  for (wchar_t *P = Path; *P; ++P)
    if (*P == L'\\' || *P == L'/')
      Last = P;
  return Last;
}

/// Skip past argv[0] in a raw command line string.
///
/// Windows command lines are a single string; argv[0] may be quoted.
/// Returns a pointer to the first character after argv[0] and any
/// trailing whitespace.
static wchar_t *SkipArgv0(wchar_t *CmdLine) {
  wchar_t *P = CmdLine;
  if (*P == L'"') {
    ++P;
    while (*P && *P != L'"')
      ++P;
    if (*P == L'"')
      ++P;
  } else {
    while (*P && *P != L' ' && *P != L'\t')
      ++P;
  }
  while (*P == L' ' || *P == L'\t')
    ++P;
  return P;
}

/// Write a static error message to stderr.  No formatting -- just a fixed
/// wide string.  Uses WriteFile on the raw stderr handle so we don't need
/// any CRT I/O functions.
static void WriteError(const wchar_t *Msg) {
  HANDLE Err = GetStdHandle(STD_ERROR_HANDLE);
  if (Err == INVALID_HANDLE_VALUE)
    return;

  // Convert to UTF-8 on the stack for WriteFile (console or pipe).
  // For a short fixed message, WideCharToMultiByte + WriteFile is simpler
  // than trying to use WriteConsoleW (which only works on real consoles).
  char Buf[256];
  int Len = WideCharToMultiByte(CP_UTF8, 0, Msg, -1, Buf, sizeof(Buf), nullptr,
                                nullptr);
  if (Len > 0)
    WriteFile(Err, Buf, static_cast<DWORD>(Len - 1), nullptr, nullptr);
}

// -- Custom entry point (no CRT) ---------------------------------------------
//
// By using a custom entry point, we bypass the entire CRT startup sequence
// (security cookie, atexit tables, locale init, thread-local storage, SEH
// setup, etc.).  The only DLL in the import table is kernel32.dll, which is
// always mapped before the process entry point runs.

#if defined(__clang__) || defined(__GNUC__)
// Clang/GCC: the entry point name is set via -e in the linker flags.
#else
// MSVC: set via pragma so it works regardless of CMake generator.
#pragma comment(linker, "/ENTRY:StubMain")
#pragma comment(linker, "/NODEFAULTLIB")
#endif

extern "C" int __stdcall StubMain() {
  // Step 1: Get the full path to this stub executable.
  wchar_t StubPath[MAX_PATH];
  DWORD Len = GetModuleFileNameW(nullptr, StubPath, MAX_PATH);
  if (Len == 0 || Len >= MAX_PATH) {
    WriteError(L"llvm-driver-stub: GetModuleFileNameW failed\n");
    ExitProcess(1);
  }

  // Step 2: Extract the stub filename for argv[0], then rewrite StubPath
  //         in-place to point to llvm.exe in the same directory.
  wchar_t *Sep = FindLastSep(StubPath);
  if (!Sep) {
    WriteError(L"llvm-driver-stub: cannot determine directory\n");
    ExitProcess(1);
  }
  wchar_t *StubName = Sep + 1; // e.g. "clang-cl.exe"

  // Save the stub filename before we overwrite it.  We need it for argv[0]
  // in the command line we'll build later.
  wchar_t StubNameBuf[MAX_PATH];
  DWORD StubNameLen = WStrLen(StubName);
  if (StubNameLen >= MAX_PATH)
    ExitProcess(1);
  WMemCpy(StubNameBuf, StubName, StubNameLen + 1); // Include NUL.

  // Step 3: Overwrite the filename portion of StubPath with "llvm.exe".
  //         StubPath was e.g. "C:\...\clang-cl.exe" (stub renamed at install)
  //         Now becomes       "C:\...\llvm.exe"
  //         "llvm.exe" (8 chars + NUL) is shorter than any alias name, so
  //         this always fits within the original MAX_PATH buffer.
  static const wchar_t LlvmExe[] = L"llvm.exe";
  WMemCpy(StubName, LlvmExe, sizeof(LlvmExe) / sizeof(wchar_t));
  // StubPath now holds the full path to llvm.exe.

  // Step 4: Build the command line.
  //
  // We need argv[0] to be the stub's filename (e.g. "clang-cl.exe") so that
  // llvm.exe dispatches to the correct tool.  The remaining arguments are
  // forwarded verbatim from the original command line.
  //
  // Layout: "<stub-name>" <remaining-args>\0
  wchar_t *OrigArgs = SkipArgv0(GetCommandLineW());

  DWORD OrigArgsLen = WStrLen(OrigArgs);
  // 1 (quote) + name + 1 (quote) + 1 (space) + args + 1 (NUL)
  DWORD CmdLineLen = 1 + StubNameLen + 1 + 1 + OrigArgsLen + 1;

  // Use the stack for typical command lines.  For very long ones (> 1024
  // wchars), fall back to VirtualAlloc -- committed pages are guaranteed
  // zero-initialized by the OS, and we avoid pulling in HeapAlloc/HeapFree.
  wchar_t StackBuf[1024];
  wchar_t *CmdLine;
  if (CmdLineLen <= sizeof(StackBuf) / sizeof(wchar_t)) {
    CmdLine = StackBuf;
  } else {
    CmdLine = static_cast<wchar_t *>(
        VirtualAlloc(nullptr, CmdLineLen * sizeof(wchar_t),
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!CmdLine) {
      WriteError(L"llvm-driver-stub: out of memory\n");
      ExitProcess(1);
    }
  }

  // Build: "<StubNameBuf> <OrigArgs>"  or  "<StubNameBuf>"
  wchar_t *W = CmdLine;
  *W++ = L'"';
  WMemCpy(W, StubNameBuf, StubNameLen);
  W += StubNameLen;
  *W++ = L'"';
  if (OrigArgsLen > 0) {
    *W++ = L' ';
    WMemCpy(W, OrigArgs, OrigArgsLen);
    W += OrigArgsLen;
  }
  *W = L'\0';

  // Step 5: Spawn llvm.exe.
  //
  // lpApplicationName = path to llvm.exe (the actual binary to execute).
  // lpCommandLine     = "<stub-name> <args>" (argv[0] for tool dispatch).
  //
  // bInheritHandles = TRUE so that stdin/stdout/stderr are forwarded.
  // The child shares the parent's console, so Ctrl+C is naturally forwarded.
  STARTUPINFOW SI;
  // Avoid ZeroMemory/memset -- not available in /NODEFAULTLIB builds.
  for (unsigned char *P = reinterpret_cast<unsigned char *>(&SI),
                     *E = P + sizeof(SI);
       P != E; ++P)
    *static_cast<volatile unsigned char *>(P) = 0;
  SI.cb = sizeof(SI);
  SI.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  SI.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  SI.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  SI.dwFlags = STARTF_USESTDHANDLES;

  PROCESS_INFORMATION PI;
  for (unsigned char *P = reinterpret_cast<unsigned char *>(&PI),
                     *E = P + sizeof(PI);
       P != E; ++P)
    *static_cast<volatile unsigned char *>(P) = 0;

  if (!CreateProcessW(StubPath, CmdLine, nullptr, nullptr,
                      /*bInheritHandles=*/TRUE, 0, nullptr, nullptr, &SI,
                      &PI)) {
    WriteError(L"llvm-driver-stub: cannot launch llvm.exe\n");
    ExitProcess(1);
  }

  // Step 6: Wait for the child and forward its exit code.
  WaitForSingleObject(PI.hProcess, INFINITE);
  DWORD ExitCode = 1;
  GetExitCodeProcess(PI.hProcess, &ExitCode);
  CloseHandle(PI.hProcess);
  CloseHandle(PI.hThread);

  // No explicit VirtualFree needed -- ExitProcess tears down the address space.
  ExitProcess(ExitCode);
}
