set(LLVM_LINK_COMPONENTS
  ${LLVM_TARGETS_TO_BUILD}
  Analysis
  BinaryFormat
  BitReader
  BitWriter
  Core
  FrontendOffloading
  IRReader
  Linker
  MC
  Option
  Object
  Support
  Target
  TargetParser
  TransformUtils
  )

set(LLVM_TARGET_DEFINITIONS SYCLLinkOpts.td)
tablegen(LLVM SYCLLinkOpts.inc -gen-opt-parser-defs)
add_public_tablegen_target(SYCLLinkerOpts)

if(NOT CLANG_BUILT_STANDALONE)
  set(tablegen_deps intrinsics_gen SYCLLinkerOpts)
endif()

add_clang_tool(clang-sycl-linker
  ClangSYCLLinker.cpp

  DEPENDS
  ${tablegen_deps}

  GENERATE_DRIVER
  )

set(CLANG_SYCL_LINKER_LIB_DEPS
  clangBasic
  )

clang_target_link_libraries(clang-sycl-linker
  PRIVATE
  ${CLANG_SYCL_LINKER_LIB_DEPS}
  )
