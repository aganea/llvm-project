//===- ScopedHandle.h - Common Windows Include File -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A helper that keeps a Windows HANDLE open. This helper is meant to be used
// in situations where it is not desirable to include windows.h.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_SCOPEDHANDLE_H
#define LLVM_SUPPORT_SCOPEDHANDLE_H

namespace llvm {
namespace sys {
namespace windows {

template <typename HandleTraits> class ScopedHandle {
  typedef typename HandleTraits::handle_type handle_type;
  handle_type Handle = HandleTraits::GetInvalid();

  ScopedHandle(const ScopedHandle &other) = delete;
  void operator=(const ScopedHandle &other) = delete;

public:
  ScopedHandle() = default;

  explicit ScopedHandle(handle_type h) : Handle(h) {}

  ~ScopedHandle() {
    if (Handle != HandleTraits::GetInvalid())
      HandleTraits::Close(Handle);
  }

  handle_type take() {
    handle_type t = Handle;
    Handle = HandleTraits::GetInvalid();
    return t;
  }

  ScopedHandle &operator=(handle_type h) {
    if (Handle != HandleTraits::GetInvalid())
      HandleTraits::Close(Handle);
    Handle = h;
    return *this;
  }

  // True if Handle is valid.
  explicit operator bool() const {
    return Handle != HandleTraits::GetInvalid();
  }

  operator handle_type() const { return Handle; }
};

struct CommonHandleTraits {
  // Don't use HANDLE here to avoid including windows.h.
  using handle_type = uint64_t;

  static handle_type GetInvalid() { return (uint64_t)-1ULL; }

  static void Close(handle_type h);
};

} // end namespace windows.
} // end namespace sys
} // end namespace llvm.

#endif
