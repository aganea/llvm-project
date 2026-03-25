set(LLVM_LINK_COMPONENTS
  BitReader
  BitWriter
  Core
  DebugInfoCodeView
  DebugInfoMSF
  DebugInfoPDB
  Object
  Support
)

add_llvm_tool(llvm-dyndbg
  llvm-dyndbg.cpp
)
