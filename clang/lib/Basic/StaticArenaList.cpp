//===--- StaticArenaList.cpp - Static arena selection filter --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Basic/StaticArenaList.h"
#include "llvm/Support/SpecialCaseList.h"
#include "llvm/Support/VirtualFileSystem.h"

using namespace clang;

namespace clang {

class StaticArenaSpecialCaseList : public llvm::SpecialCaseList {
public:
  static std::unique_ptr<StaticArenaSpecialCaseList>
  create(const std::vector<std::string> &Paths, llvm::vfs::FileSystem &VFS,
         std::string &Error);

  bool isEmpty() const { return sections().empty(); }
};

std::unique_ptr<StaticArenaSpecialCaseList>
StaticArenaSpecialCaseList::create(const std::vector<std::string> &Paths,
                                   llvm::vfs::FileSystem &VFS,
                                   std::string &Error) {
  auto SCL = std::make_unique<StaticArenaSpecialCaseList>();
  if (SCL->createInternal(Paths, VFS, Error))
    return SCL;
  return nullptr;
}

} // namespace clang

std::unique_ptr<StaticArenaList>
StaticArenaList::create(ArrayRef<std::string> Paths, llvm::vfs::FileSystem &VFS,
                        StringRef SectionName, std::string &Error) {
  auto SCL = StaticArenaSpecialCaseList::create(Paths, VFS, Error);
  if (!SCL)
    return nullptr;
  return std::unique_ptr<StaticArenaList>(
      new StaticArenaList(std::move(SCL), SectionName));
}

StaticArenaList::StaticArenaList(
    std::unique_ptr<StaticArenaSpecialCaseList> SCL, StringRef SectionName)
    : SCL(std::move(SCL)), SectionName(SectionName),
      Empty(this->SCL->isEmpty()) {}

StaticArenaList::~StaticArenaList() = default;

bool StaticArenaList::isTypeSelected(StringRef TypeName) const {
  return SCL->inSection(SectionName, "type", TypeName);
}

bool StaticArenaList::isFileSelected(StringRef FileName) const {
  return SCL->inSection(SectionName, "src", FileName);
}

bool StaticArenaList::isNameSelected(StringRef Name) const {
  return SCL->inSection(SectionName, "name", Name);
}

bool StaticArenaList::isGlobalSelected(StringRef MangledName) const {
  return SCL->inSection(SectionName, "global", MangledName);
}
