set(LLVM_LINK_COMPONENTS
  Core
  Support
  Object
  Coverage
  ProfileData
  TargetParser
  )

add_llvm_tool(llvm-cov
  llvm-cov.cpp
  gcov.cpp
  CodeCoverage.cpp
  CoverageExporterJson.cpp
  CoverageExporterLcov.cpp
  CoverageFilters.cpp
  CoverageReport.cpp
  CoverageSummaryInfo.cpp
  SourceCoverageView.cpp
  SourceCoverageViewHTML.cpp
  SourceCoverageViewText.cpp
  TestingSupport.cpp

  GENERATE_DRIVER_REQUIRES_PER_INVOCATION_GLOBALS
  )

if(NOT LLVM_TOOL_LLVM_DRIVER_BUILD OR
   NOT LLVM_DRIVER_PER_INVOCATION_GLOBALS OR
   (LLVM_DISTRIBUTION_COMPONENTS AND
    NOT llvm-cov IN_LIST LLVM_DISTRIBUTION_COMPONENTS))
  target_link_libraries(llvm-cov PRIVATE LLVMHTTP LLVMDebuginfod)
endif()
