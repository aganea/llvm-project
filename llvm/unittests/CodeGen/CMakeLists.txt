set(LLVM_LINK_COMPONENTS
  ${LLVM_TARGETS_TO_BUILD}
  Analysis
  AsmParser
  AsmPrinter
  CodeGen
  CodeGenTypes
  Core
  FileCheck
  IRPrinter
  MC
  MIRParser
  Passes
  ScalarOpts
  SelectionDAG
  Support
  Target
  TargetParser
  TransformUtils
  )

set(LLVM_OPTIONAL_SOURCES
  StaticArenaCommandFlagsTest.cpp
  )

add_llvm_unittest(CodeGenTests
  AccelTableTest.cpp
  AllocationOrderTest.cpp
  AMDGPUMetadataTest.cpp
  AsmPrinterDwarfTest.cpp
  CCStateTest.cpp
  DIEHashTest.cpp
  DIETest.cpp
  DroppedVariableStatsMIRTest.cpp
  DwarfStringPoolEntryRefTest.cpp
  GCMetadata.cpp
  InstrRefLDVTest.cpp
  LowLevelTypeTest.cpp
  LexicalScopesTest.cpp
  MachinePassRegistryTest.cpp
  MachineBasicBlockTest.cpp
  MachineDomTreeUpdaterTest.cpp
  MachineInstrBundleIteratorTest.cpp
  MachineInstrTest.cpp
  MachineModuleInfoTest.cpp
  MachineOperandTest.cpp
  MIR2VecTest.cpp
  RegAllocBasicTest.cpp
  RegAllocScoreTest.cpp
  RegisterPressureTest.cpp
  RegisterTest.cpp
  PassManagerTest.cpp
  RematerializerTest.cpp
  ScalableVectorMVTsTest.cpp
  SchedBoundary.cpp
  SelectionDAGAddressAnalysisTest.cpp
  SelectionDAGCSETest.cpp
  SelectionDAGNodeConstructionTest.cpp
  SelectionDAGPatternMatchTest.cpp
  TypeTraitsTest.cpp
  TargetOptionsTest.cpp
  TestAsmPrinter.cpp
  X86MCInstLowerTest.cpp
  )

add_subdirectory(GlobalISel)
add_subdirectory(CGPluginTest)

target_link_libraries(CodeGenTests PRIVATE LLVMTestingSupport)

# The static-arena driver configuration needs one proof using the real
# CommandFlags.cpp implementation, rather than only a fixture that models its
# cached *View pointers.  Build a private copy with the same producer flags as
# llvm-driver's component-library copies; the ordinary LLVMCodeGen archive is
# still linked for the rest of the implementation, but its CommandFlags object
# is not extracted because this object satisfies those symbols first.
if(LLVM_DRIVER_PER_INVOCATION_GLOBALS)
  add_library(StaticArenaCommandFlagsObject OBJECT EXCLUDE_FROM_ALL
    ${LLVM_MAIN_SRC_DIR}/lib/CodeGen/CommandFlags.cpp)
  llvm_update_compile_flags(StaticArenaCommandFlagsObject)
  target_compile_options(StaticArenaCommandFlagsObject PRIVATE
    "SHELL:-Xclang -fstatic-arena=llvmi_static_arena_command_flags_test"
    "SHELL:-Xclang \"-fstatic-arena-list=${LLVM_DRIVER_STATIC_ARENA_LIST}\"")
  add_dependencies(StaticArenaCommandFlagsObject intrinsics_gen)
  llvm_add_driver_producer_list_dependencies(StaticArenaCommandFlagsObject)

  add_llvm_unittest(StaticArenaCommandFlagsTests
    StaticArenaCommandFlagsTest.cpp)
  target_sources(StaticArenaCommandFlagsTests PRIVATE
    $<TARGET_OBJECTS:StaticArenaCommandFlagsObject>)
  target_link_libraries(StaticArenaCommandFlagsTests PRIVATE LLVMTestingSupport)
endif()
