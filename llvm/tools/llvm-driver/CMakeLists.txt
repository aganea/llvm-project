get_property(LLVM_COMMON_DEPENDS GLOBAL PROPERTY LLVM_DRIVER_DEPS)
get_property(LLVM_DRIVER_OBJLIBS GLOBAL PROPERTY LLVM_DRIVER_OBJLIBS)

get_property(LLVM_DRIVER_TOOLS GLOBAL PROPERTY LLVM_DRIVER_TOOLS)

list(SORT LLVM_DRIVER_TOOLS)
list(REVERSE LLVM_DRIVER_TOOLS)
foreach(tool ${LLVM_DRIVER_TOOLS})
  string(REPLACE "-" "_" tool_entry ${tool})
  get_property(tool_aliases GLOBAL PROPERTY LLVM_DRIVER_TOOL_ALIASES_${tool})
  foreach(alias ${tool_aliases})
    set_property(GLOBAL APPEND PROPERTY LLVM_DRIVER_TOOL_SYMLINKS ${alias})
    string(REPLACE "llvm-" "" alias ${alias})
    set(def_decl "${def_decl}LLVM_DRIVER_TOOL(\"${alias}\", ${tool_entry})\n")
  endforeach()
  get_property(hidden_tool_aliases GLOBAL PROPERTY LLVM_DRIVER_HIDDEN_TOOL_ALIASES_${tool})
  foreach(alias ${hidden_tool_aliases})
    string(REPLACE "llvm-" "" alias ${alias})
    set(def_decl "${def_decl}LLVM_DRIVER_TOOL(\"${alias}\", ${tool_entry})\n")
  endforeach()
endforeach()

file(WRITE
          "${CMAKE_CURRENT_BINARY_DIR}/LLVMDriverTools.def"
          "${def_decl}#undef LLVM_DRIVER_TOOL\n")

# Generate a plain-text manifest mapping each tool alias in bin/ to the real
# executable it stands in for: one "<alias> <target>" pair per line. This is
# used by llvm-link-mode.ps1 to know which executables in bin/ are aliases,
# and of what, regardless of whether they are currently copies, hard links,
# symlinks, or stubs.
#
# <target> is "llvm" for every llvm-driver alias (llvm.exe). A handful of
# aliases duplicate a *different* standalone tool that llvm-driver doesn't
# fold in at all -- e.g. flang-new is an alias of flang.exe, not llvm.exe --
# and are registered via the LLVM_NON_DRIVER_TOOL_ALIASES property instead
# (see add_clang_symlink/add_flang_symlink) so they can be materialized
# post-install the same way, instead of NSIS embedding a byte-identical
# second copy of the target.
get_property(_all_symlinks GLOBAL PROPERTY LLVM_DRIVER_TOOL_SYMLINKS)
list(SORT _all_symlinks)
set(_manifest_content "")
foreach(_alias ${_all_symlinks})
  string(APPEND _manifest_content "${_alias} llvm\n")
endforeach()

get_property(_non_driver_aliases GLOBAL PROPERTY LLVM_NON_DRIVER_TOOL_ALIASES)
list(SORT _non_driver_aliases)
foreach(_non_driver_alias ${_non_driver_aliases})
  string(REPLACE ":" ";" _non_driver_alias_list ${_non_driver_alias})
  list(GET _non_driver_alias_list 0 _alias)
  list(GET _non_driver_alias_list 1 _target)
  string(APPEND _manifest_content "${_alias} ${_target}\n")
endforeach()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/llvm-driver-tools.txt" "${_manifest_content}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/llvm-driver-tools.txt"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT llvm-driver)

# Install the link-mode switching script (Windows only).
if(WIN32)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/llvm-link-mode.ps1"
          DESTINATION "${CMAKE_INSTALL_BINDIR}"
          COMPONENT llvm-driver)
endif()

target_include_directories(llvm-driver PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_sources(llvm-driver PRIVATE llvm-driver.cpp)

set_target_properties(llvm-driver PROPERTIES OUTPUT_NAME llvm)

target_link_libraries(llvm-driver PUBLIC ${LLVM_DRIVER_OBJLIBS})
target_link_libraries(llvm-driver PUBLIC LLVMDebuginfod)

if(APPLE)
  # dsymutil uses some CoreFoundation stuff on Darwin...
  target_link_libraries(llvm-driver PRIVATE "-framework CoreFoundation")
endif(APPLE)

# LLVM_INSTALL_DRIVER_ALIASES is declared in the top-level llvm/CMakeLists.txt,
# ahead of add_subdirectory(clang)/add_subdirectory(flang), so that
# add_clang_symlink/add_flang_symlink can also read it.

macro(generate_driver_tool_targets)
  get_property(LLVM_DRIVER_TOOL_SYMLINKS GLOBAL PROPERTY LLVM_DRIVER_TOOL_SYMLINKS)
  foreach(name ${LLVM_DRIVER_TOOL_SYMLINKS})
    # Always create the build-tree aliases (used by check-* targets and manual testing).
    add_llvm_tool_symlink(${name} llvm-driver ALWAYS_GENERATE)
    if(LLVM_INSTALL_DRIVER_ALIASES)
      llvm_install_symlink(LLVM ${name} llvm-driver ALWAYS_GENERATE)
    endif()
  endforeach()
endmacro()
