# Minimal stub executable used as a fallback when hard links to llvm.exe
# cannot be created (e.g. FAT32/exFAT installs).  When invoked as e.g.
# "clang-cl.exe", it spawns llvm.exe in the same directory, passing the
# stub's filename as argv[0] for tool dispatch.
#
# This target is only built on Windows and only when LLVM_TOOL_LLVM_DRIVER_BUILD
# is enabled, since it only makes sense alongside the unified llvm-driver binary.
#
# The stub uses a custom entry point with /NODEFAULTLIB -- no CRT at all.
# The only DLL import is kernel32.dll (always pre-loaded by the OS).
# Resulting binary is ~3-4 KB.

if(NOT WIN32)
  return()
endif()

if(NOT LLVM_TOOL_LLVM_DRIVER_BUILD)
  return()
endif()

add_executable(llvm-driver-stub llvm-driver-stub.cpp)

# No LLVM library dependencies -- the stub is self-contained.
set_target_properties(llvm-driver-stub PROPERTIES
  OUTPUT_NAME "llvm-driver-stub"
  RUNTIME_OUTPUT_DIRECTORY "${LLVM_RUNTIME_OUTPUT_INTDIR}"
)

# Link only against kernel32 -- no CRT, no other system libraries.
target_link_libraries(llvm-driver-stub PRIVATE kernel32)

if(MSVC OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
  # MSVC or clang-cl: /NODEFAULTLIB strips all default libraries,
  # /ENTRY sets the custom entry point.  /SUBSYSTEM:CONSOLE for console app.
  target_link_options(llvm-driver-stub PRIVATE
    /NODEFAULTLIB
    /ENTRY:StubMain
    /SUBSYSTEM:CONSOLE
  )
  # Minimize binary size.
  target_compile_options(llvm-driver-stub PRIVATE /Os /GS-)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  # GCC/Clang (MinGW): equivalent flags.
  target_link_options(llvm-driver-stub PRIVATE
    -nostdlib
    -e StubMain
    -Wl,--subsystem,console
    -s
  )
  target_compile_options(llvm-driver-stub PRIVATE -Os -fno-stack-protector)
endif()

# Install alongside llvm.exe in the bin directory.
install(TARGETS llvm-driver-stub
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  COMPONENT llvm-driver-stub
)
