set(LLVM_LINK_COMPONENTS
  Core
  Object
  ProfileData
  Support
  )

add_llvm_tool(llvm-profdata
  llvm-profdata.cpp

  DEPENDS
  intrinsics_gen
  GENERATE_DRIVER_REQUIRES_PER_INVOCATION_GLOBALS
  )

if(NOT LLVM_TOOL_LLVM_DRIVER_BUILD OR
   NOT LLVM_DRIVER_PER_INVOCATION_GLOBALS OR
   (LLVM_DISTRIBUTION_COMPONENTS AND
    NOT llvm-profdata IN_LIST LLVM_DISTRIBUTION_COMPONENTS))
  target_link_libraries(llvm-profdata PRIVATE LLVMDebuginfod LLVMHTTP)
endif()
