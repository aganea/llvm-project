//===- WindowsSupport.h - Common Windows Include File -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines things specific to Windows implementations.  In addition to
// providing some helpers for working with win32 APIs, this header wraps
// <windows.h> with some portability macros.  Always include WindowsSupport.h
// instead of including <windows.h> directly.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
//=== WARNING: Implementation here must contain only generic Win32 code that
//===          is guaranteed to work on *all* Win32 variants.
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_WINDOWSSUPPORT_H
#define LLVM_SUPPORT_WINDOWSSUPPORT_H

// mingw-w64 tends to define it as 0x0502 in its headers.
#undef _WIN32_WINNT
#undef _WIN32_IE

// Require at least Windows 7 API.
#define _WIN32_WINNT 0x0601
#define _WIN32_IE    0x0800 // MinGW at it again. FIXME: verify if still needed.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Config/llvm-config.h" // Get build system configuration settings
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/VersionTuple.h"
#include "llvm/Support/Windows/ScopedHandle.h"
#include <cassert>
#include <string>
#include <system_error>
#include <windows.h>

// Must be included after windows.h
#include <wincrypt.h>

namespace llvm {

using namespace sys::windows;

/// Determines if the program is running on Windows 8 or newer. This
/// reimplements one of the helpers in the Windows 8.1 SDK, which are intended
/// to supercede raw calls to GetVersionEx. Old SDKs, Cygwin, and MinGW don't
/// yet have VersionHelpers.h, so we have our own helper.
bool RunningWindows8OrGreater();

/// Determines if the program is running on Windows 11 or Windows Server 2022.
bool RunningWindows11OrGreater();

/// Returns the Windows version as Major.Minor.0.BuildNumber. Uses
/// RtlGetVersion or GetVersionEx under the hood depending on what is available.
/// GetVersionEx is deprecated, but this API exposes the build number which can
/// be useful for working around certain kernel bugs.
llvm::VersionTuple GetWindowsOSVersion();

bool MakeErrMsg(std::string *ErrMsg, const std::string &prefix);

// Include GetLastError() in a fatal error message.
[[noreturn]] inline void ReportLastErrorFatal(const char *Msg) {
  std::string ErrMsg;
  MakeErrMsg(&ErrMsg, Msg);
  llvm::report_fatal_error(Twine(ErrMsg));
}

struct TypedHandleTraits {
  using handle_type = HANDLE;

  static handle_type GetInvalid() { return INVALID_HANDLE_VALUE; }

  static void Close(handle_type h) {
    CommonHandleTraits::Close((CommonHandleTraits::handle_type)h);
  }
};

struct JobHandleTraits : TypedHandleTraits {
  static handle_type GetInvalid() { return NULL; }
};

struct CryptContextTraits : TypedHandleTraits {
  using handle_type = HCRYPTPROV;

  static handle_type GetInvalid() { return 0; }

  static void Close(handle_type h) { ::CryptReleaseContext(h, 0); }
};

struct RegTraits : TypedHandleTraits {
  using handle_type = HKEY;

  static handle_type GetInvalid() { return NULL; }

  static void Close(handle_type h) { ::RegCloseKey(h); }
};

struct FindHandleTraits : TypedHandleTraits {
  static void Close(handle_type h) { ::FindClose((HANDLE)h); }
};

struct FileHandleTraits : TypedHandleTraits {};

typedef ScopedHandle<TypedHandleTraits> ScopedCommonHandle;
typedef ScopedHandle<FileHandleTraits> ScopedFileHandle;
typedef ScopedHandle<CryptContextTraits> ScopedCryptContext;
typedef ScopedHandle<RegTraits> ScopedRegHandle;
typedef ScopedHandle<FindHandleTraits> ScopedFindHandle;
typedef ScopedHandle<JobHandleTraits> ScopedJobHandle;

template <class T>
class SmallVectorImpl;

template <class T>
typename SmallVectorImpl<T>::const_pointer
c_str(SmallVectorImpl<T> &str) {
  str.push_back(0);
  str.pop_back();
  return str.data();
}

namespace sys {

inline std::chrono::nanoseconds toDuration(FILETIME Time) {
  ULARGE_INTEGER TimeInteger;
  TimeInteger.LowPart = Time.dwLowDateTime;
  TimeInteger.HighPart = Time.dwHighDateTime;

  // FILETIME's are # of 100 nanosecond ticks (1/10th of a microsecond)
  return std::chrono::nanoseconds(100 * TimeInteger.QuadPart);
}

inline TimePoint<> toTimePoint(FILETIME Time) {
  ULARGE_INTEGER TimeInteger;
  TimeInteger.LowPart = Time.dwLowDateTime;
  TimeInteger.HighPart = Time.dwHighDateTime;

  // Adjust for different epoch
  TimeInteger.QuadPart -= 11644473600ll * 10000000;

  // FILETIME's are # of 100 nanosecond ticks (1/10th of a microsecond)
  return TimePoint<>(std::chrono::nanoseconds(100 * TimeInteger.QuadPart));
}

inline FILETIME toFILETIME(TimePoint<> TP) {
  ULARGE_INTEGER TimeInteger;
  TimeInteger.QuadPart = TP.time_since_epoch().count() / 100;
  TimeInteger.QuadPart += 11644473600ll * 10000000;

  FILETIME Time;
  Time.dwLowDateTime = TimeInteger.LowPart;
  Time.dwHighDateTime = TimeInteger.HighPart;
  return Time;
}

namespace windows {
// Returns command line arguments. Unlike arguments given to main(),
// this function guarantees that the returned arguments are encoded in
// UTF-8 regardless of the current code page setting.
std::error_code GetCommandLineArguments(SmallVectorImpl<const char *> &Args,
                                        BumpPtrAllocator &Alloc);

/// Convert UTF-8 path to a suitable UTF-16 path for use with the Win32 Unicode
/// File API.
std::error_code widenPath(const Twine &Path8, SmallVectorImpl<wchar_t> &Path16,
                          size_t MaxPathLen = MAX_PATH);

} // end namespace windows
} // end namespace sys
} // end namespace llvm.

#endif
