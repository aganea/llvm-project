add_lldb_library(lldbPluginStructuredDataDynDbg PLUGIN
  StructuredDataDynDbg.cpp
  DynDbgDeoptimizer.cpp

  LINK_LIBS
    lldbCore
    lldbHost
    lldbInterpreter
    lldbSymbol
    lldbTarget
    lldbUtility

  LINK_COMPONENTS
    BitReader
    BitWriter
    Core
    DebugInfoCodeView
    DebugInfoMSF
    DebugInfoPDB
    Object
    Support
  )
