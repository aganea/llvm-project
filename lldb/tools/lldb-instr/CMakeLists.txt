add_lldb_utility(lldb-instr
  Instrument.cpp

  LINK_COMPONENTS
    Support
  CLANG_LIBS
    clangAST
    clangBasic
    clangCodeGen
    clangFrontend
    clangLex
    clangRewrite
    clangSerialization
    clangTooling
  )
