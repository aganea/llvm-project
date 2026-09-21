# On AIX, enable run-time linking to allow symbols from the plugins shared
# objects to be properly bound.
if(CMAKE_SYSTEM_NAME STREQUAL "AIX")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-brtl")
endif()

set(LLVM_LINK_COMPONENTS
  Passes
  Support
  )

# Keep the signal-slot reuse test separate from death tests. On Windows, an
# already-installed LLVM exception handler interferes with gtest's child
# process used to implement a later EXPECT_DEATH.
add_llvm_unittest(PrintCrashIRInstrumentationTests
  PARTIAL_SOURCES_INTENDED
  PrintCrashIRInstrumentationTest.cpp
  )

add_llvm_unittest(PrintCrashIRInstrumentationDeathTests
  PARTIAL_SOURCES_INTENDED
  PrintCrashIRInstrumentationDeathTest.cpp
  )

add_subdirectory(PassBuilderBindings)
add_subdirectory(Plugins)
