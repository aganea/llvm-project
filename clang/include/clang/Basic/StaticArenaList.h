//===--- StaticArenaList.h - Static arena selection filter ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// User-provided filter selecting which globals live in the static arena.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_BASIC_STATICARENALIST_H
#define LLVM_CLANG_BASIC_STATICARENALIST_H

#include "clang/Basic/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <memory>
#include <string>

namespace llvm {
namespace vfs {
class FileSystem;
}
} // namespace llvm

namespace clang {

class StaticArenaSpecialCaseList;

/// Selects globals whose storage and lifecycle are owned by a static arena.
///
/// The file uses the sanitizer special case list format, under an "arena"
/// section:
/// \code
///   [arena]
///   type:llvm::cl::opt
///   type:llvm::cl::OptionCategory
///   src:*/llvm/tools/*
///   name:llvm::DebugFlag
///   global:?SomeSpecificGlobal@@*
/// \endcode
class StaticArenaList {
  std::unique_ptr<StaticArenaSpecialCaseList> SCL;
  std::string SectionName;
  const bool Empty;

  StaticArenaList(std::unique_ptr<StaticArenaSpecialCaseList> SCL,
                  StringRef SectionName);

public:
  /// Parses the lists in \p Paths using \p VFS. On failure, returns null and
  /// writes a diagnostic-ready description to \p Error.
  static std::unique_ptr<StaticArenaList> create(ArrayRef<std::string> Paths,
                                                 llvm::vfs::FileSystem &VFS,
                                                 StringRef SectionName,
                                                 std::string &Error);
  ~StaticArenaList();

  bool isEmpty() const { return Empty; }

  /// Matches `type:<glob>`. \p TypeName is the qualified name of the global's
  /// record type with template arguments dropped, so that one `llvm::cl::opt`
  /// entry covers every cl::opt<T> specialization.
  bool isTypeSelected(StringRef TypeName) const;

  /// Matches `src:<glob>` -- the whole-translation-unit escape hatch.
  bool isFileSelected(StringRef FileName) const;

  /// Matches `name:<glob>` against a declaration's unqualified source
  /// identifier, qualified source name, or `<source-file>@<name>`. The latter
  /// form disambiguates common file-local identifiers without depending on
  /// target mangling.
  bool isNameSelected(StringRef Name) const;

  /// Matches `global:<glob>` -- the mangled-name escape hatch, for cases a
  /// type does not describe.
  bool isGlobalSelected(StringRef MangledName) const;
};

} // namespace clang

#endif
