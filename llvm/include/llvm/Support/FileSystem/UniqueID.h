//===- llvm/Support/FileSystem/UniqueID.h - UniqueID for files --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is cut out of llvm/Support/FileSystem.h to allow UniqueID to be
// reused without bloating the includes.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_FILESYSTEM_UNIQUEID_H
#define LLVM_SUPPORT_FILESYSTEM_UNIQUEID_H

#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/Hashing.h"
#include <cstdint>
#include <utility>

#if defined(_WIN32)
#include "llvm/Support/Windows/ScopedHandle.h"
#include <memory>
#undef max
#endif

namespace llvm {
namespace sys {
namespace fs {

class UniqueID {
  uint64_t Device = 0;
  uint64_t File = 0;

#if defined(_WIN32)
  // On Windows, the handle is kept here only to guarantee that the value of
  // File remains valid during the lifetime of the UniqueID instance. The
  // underlying Windows API only guarantees valid IDs as long as the file is
  // open, please see:
  // https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_info
  // and:
  // https://learn.microsoft.com/en-us/windows/win32/api/fileapi/ns-fileapi-by_handle_file_information

  using H =
      llvm::sys::windows::ScopedHandle<llvm::sys::windows::CommonHandleTraits>;
  std::shared_ptr<H> Handle;
#endif

public:
  UniqueID() = default;

#if defined(LLVM_ON_UNIX)
  UniqueID(uint64_t Device, uint64_t File) : Device(Device), File(File) {}
#elif defined(_WIN32)
  UniqueID(uint64_t VolumeSerialNumber, uint64_t FileIndex, uint64_t Handle)
      : Device(VolumeSerialNumber), File(FileIndex),
        Handle(std::make_shared<H>(Handle)) {}
#endif

  bool operator==(const UniqueID &Other) const {
    return Device == Other.Device && File == Other.File;
  }
  bool operator!=(const UniqueID &Other) const { return !(*this == Other); }
  bool operator<(const UniqueID &Other) const {
    /// Don't use std::tie since it bloats the compile time of this header.
    if (Device < Other.Device)
      return true;
    if (Other.Device < Device)
      return false;
    return File < Other.File;
  }

  uint64_t getDevice() const { return Device; }
  uint64_t getFile() const { return File; }

  // Create a UniqueID that is known to refer to in-memory files. The FileID
  // argument must be a unique value representing a in-memory file.
  static UniqueID inMemory(uint64_t DeviceID, uint64_t FileID) {
#if defined(LLVM_ON_UNIX)
    return UniqueID(DeviceID, FileID);
#elif defined(_WIN32)
    UniqueID ID;
    ID.Device = DeviceID;
    ID.File = FileID;
    return ID;
#endif
  }

  hash_code getHashValue() const {
    return hash_value(std::make_pair(Device, File));
  }

private:
  UniqueID(uint64_t Key) : Device(Key), File(Key) {}

  friend struct DenseMapInfo<llvm::sys::fs::UniqueID>;
};

} // end namespace fs
} // end namespace sys

// Support UniqueIDs as DenseMap keys.
template <> struct DenseMapInfo<llvm::sys::fs::UniqueID> {
  static inline llvm::sys::fs::UniqueID getEmptyKey() {
    auto EmptyKey = DenseMapInfo<uint64_t>::getEmptyKey();
    return llvm::sys::fs::UniqueID(EmptyKey);
  }

  static inline llvm::sys::fs::UniqueID getTombstoneKey() {
    auto TombstoneKey = DenseMapInfo<uint64_t>::getTombstoneKey();
    return llvm::sys::fs::UniqueID(TombstoneKey);
  }

  static hash_code getHashValue(const llvm::sys::fs::UniqueID &Tag) {
    return Tag.getHashValue();
  }

  static bool isEqual(const llvm::sys::fs::UniqueID &LHS,
                      const llvm::sys::fs::UniqueID &RHS) {
    return LHS == RHS;
  }
};

} // end namespace llvm

#endif // LLVM_SUPPORT_FILESYSTEM_UNIQUEID_H
