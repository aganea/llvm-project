add_llvm_library(LLVMDebugInfoSymbolServer
  PDBSymbolDownloader.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/DebugInfo/SymbolServer

  LINK_COMPONENTS
  Support
  SupportHTTP
  DebugInfoPDB
  BinaryFormat
  Object
)