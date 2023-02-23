//===- ParallelLink.cpp -----------------------------------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lld/Common/Driver.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/ThreadPool.h"
#include "gmock/gmock.h"

LLD_HAS_DRIVER(coff);

using namespace llvm;
using namespace lld;

static const char *expand(const char *path, StringSaver &saver) {
  StringRef thisFile = sys::path::parent_path(__FILE__);
  if (!StringRef(path).contains("%"))
    return path;

  std::string expanded = path;

  size_t s = expanded.find("%S");
  if (s != std::string::npos) {
    expanded.replace(s, 2, thisFile.data(), thisFile.size());
  }

  size_t coff = expanded.find("%coff_tests");
  if (coff != std::string::npos) {
    std::string coff_tests = thisFile.str();
    coff_tests += "\\..\\..\\test\\COFF";
    expanded.replace(coff, 11, coff_tests.data(), coff_tests.size());
  }

  size_t t = expanded.find("%t");
  if (t != std::string::npos) {
    Expected<sys::fs::TempFile> file =
        sys::fs::TempFile::create("lld-temp-%%%%%%");
    if (!file)
      consumeError(file.takeError());

    expanded.replace(t, 2, file->TmpName.data(), file->TmpName.size());
    consumeError(file->keep());
  }

  return saver.save(expanded).data();
}

static bool lldInvoke(std::vector<const char *> args) {
  BumpPtrAllocator alloc;
  StringSaver saver{alloc};
  for (auto &a : args) {
    if (StringRef(a).contains("%")) {
      a = expand(a, saver);
    }
  }
  Result r = lldMain(args, outs(), errs(), {{Flavor::WinLink, &coff::link}});
  return !r.retCode && r.canRunAgain;
}

TEST(AsLib, Parallel) {
  std::vector<const char *> cmd1{"lld-link",
                                 "%coff_tests/Inputs/precomp-a.obj",
                                 "%coff_tests/Inputs/precomp-b.obj",
                                 "%coff_tests/Inputs/precomp.obj",
                                 "/nodefaultlib",
                                 "/entry:main",
                                 "/debug:noghash",
                                 "/pdb:%t.pdb",
                                 "/out:%t.exe",
                                 "/opt:ref",
                                 "/opt:icf"};
  std::vector<const char *> cmd2{"lld-link",
                                 "%coff_tests/Inputs/precomp-ghash-precomp.obj",
                                 "%coff_tests/Inputs/precomp-ghash-obj1.obj",
                                 "%coff_tests/Inputs/precomp-ghash-obj2.obj",
                                 "/debug:ghash",
                                 "/out:%t.exe",
                                 "/pdb:%t.pdb",
                                 "/nodefaultlib",
                                 "/force"};

  llvm::setGlobalTPStrategy(hardware_concurrency(2));
  ThreadPool &tp = llvm::getGlobalTP();
  for (unsigned i = 0; i < 2; ++i) {
    tp.async([&]() { EXPECT_TRUE(lldInvoke(cmd1)); });
    tp.async([&]() { EXPECT_TRUE(lldInvoke(cmd2)); });
  }
  tp.wait();
}
