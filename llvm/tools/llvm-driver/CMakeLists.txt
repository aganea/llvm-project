get_property(LLVM_COMMON_DEPENDS GLOBAL PROPERTY LLVM_DRIVER_DEPS)
get_property(LLVM_DRIVER_OBJLIBS GLOBAL PROPERTY LLVM_DRIVER_OBJLIBS)

get_property(LLVM_DRIVER_TOOLS GLOBAL PROPERTY LLVM_DRIVER_TOOLS)

target_include_directories(llvm-driver PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_sources(llvm-driver PRIVATE llvm-driver.cpp)

set_target_properties(llvm-driver PROPERTIES OUTPUT_NAME llvm)

target_link_libraries(llvm-driver PUBLIC ${LLVM_DRIVER_OBJLIBS})
if(LLVM_DRIVER_PER_INVOCATION_GLOBALS)
  # LLVMDebuginfod is a non-component LLVM library linked by several folded
  # tools. Keep the normal archive off the driver link entirely; its private
  # copy carries an equivalently rewritten LLVM/system link interface.  The
  # graph finalizer treats this direct driver edge as image-shared and includes
  # its lifecycle closure in every per-tool runner.
  get_property(LLVM_DRIVER_DEBUGINFOD_TARGET TARGET LLVMDebuginfod
    PROPERTY LLVM_DRIVER_PER_INVOCATION_TARGET)
  if(NOT LLVM_DRIVER_DEBUGINFOD_TARGET)
    message(FATAL_ERROR
      "LLVMDebuginfod has no per-invocation llvm-driver archive")
  endif()
  target_link_libraries(llvm-driver PUBLIC ${LLVM_DRIVER_DEBUGINFOD_TARGET})
  # Unlike the Clang and LLD helper-added driver edges, this library is not
  # also attributed to a particular folded tool object.  Register it explicitly
  # as image-shared so every dispatch entry runs its lifecycle closure.
  set_property(TARGET llvm-driver APPEND PROPERTY
    LLVM_DRIVER_PER_INVOCATION_SHARED_LINK_ROOTS
    ${LLVM_DRIVER_DEBUGINFOD_TARGET})
  # The root is linked once by the aggregate, so llvm-debuginfod's object
  # target has no ordinary edge from which the graph finalizer can infer that
  # it executes this code. Associate only that actual consumer with the shared
  # closure for process-registry initialization classification. Other tools
  # must not inherit the root merely because it is present in the image.
  if(llvm-debuginfod IN_LIST LLVM_DRIVER_TOOLS)
    if(NOT TARGET obj.llvm-debuginfod)
      message(FATAL_ERROR
        "folded llvm-debuginfod has no object target")
    endif()
    set_property(TARGET obj.llvm-debuginfod APPEND PROPERTY
      LLVM_DRIVER_PROCESS_INITIALIZATION_SHARED_LINK_ROOTS
      ${LLVM_DRIVER_DEBUGINFOD_TARGET})
  endif()
else()
  target_link_libraries(llvm-driver PUBLIC LLVMDebuginfod)
endif()

if(APPLE)
  # dsymutil uses some CoreFoundation stuff on Darwin...
  target_link_libraries(llvm-driver PRIVATE "-framework CoreFoundation")
endif(APPLE)

if(LLVM_DRIVER_PER_INVOCATION_GLOBALS)
  # This is intentionally after every project and every direct driver link:
  # it is the first point at which the complete target graph is available.
  llvm_finalize_driver_per_invocation_graph(llvm-driver)
endif()

# The finalizer recomputes this from each tool's authoritative late link
# closure, so do not snapshot it before finalization.
get_property(LLVM_DRIVER_PER_INVOCATION_TOOLS GLOBAL PROPERTY
  LLVM_DRIVER_PER_INVOCATION_TOOLS)
get_property(LLVM_DRIVER_PROCESS_INITIALIZATION_TOOLS GLOBAL PROPERTY
  LLVM_DRIVER_PROCESS_INITIALIZATION_TOOLS)
get_property(LLVM_DRIVER_PROCESS_INITIALIZATION_CAPABILITIES GLOBAL PROPERTY
  LLVM_DRIVER_PROCESS_INITIALIZATION_CAPABILITIES)
if(LLVM_DRIVER_PROCESS_INITIALIZATION_CAPABILITIES MATCHES "-NOTFOUND$")
  set(LLVM_DRIVER_PROCESS_INITIALIZATION_CAPABILITIES)
endif()

if(LLVM_DRIVER_PER_INVOCATION_GLOBALS)
  # Recompute the producer predicate from each tool object and the authoritative
  # final closure.  A producer without a dispatch bit would make its first
  # resolver access run with no arena bound.
  foreach(tool ${LLVM_DRIVER_TOOLS})
    set(tool_reaches_per_invocation_globals FALSE)
    get_property(tool_object_is_per_invocation TARGET obj.${tool}
      PROPERTY LLVM_DRIVER_PER_INVOCATION_PRODUCER)
    if(tool_object_is_per_invocation)
      set(tool_reaches_per_invocation_globals TRUE)
    endif()
    get_property(tool_library_closure GLOBAL
      PROPERTY LLVM_DRIVER_LIBRARY_CLOSURE_${tool})
    foreach(tool_library ${tool_library_closure})
      get_property(tool_per_invocation_library TARGET ${tool_library}
        PROPERTY LLVM_DRIVER_PER_INVOCATION_TARGET)
      if(tool_per_invocation_library)
        get_property(tool_library_is_per_invocation
          TARGET ${tool_per_invocation_library}
          PROPERTY LLVM_DRIVER_PER_INVOCATION_PRODUCER)
        if(tool_library_is_per_invocation)
          set(tool_reaches_per_invocation_globals TRUE)
        endif()
      endif()
    endforeach()
    if(tool_reaches_per_invocation_globals AND
       NOT "${tool}" IN_LIST LLVM_DRIVER_PER_INVOCATION_TOOLS)
      message(FATAL_ERROR
        "llvm-driver tool ${tool} reaches per-invocation globals but lacks "
        "the corresponding dispatch bit")
    endif()
  endforeach()
endif()

list(SORT LLVM_DRIVER_TOOLS)
list(REVERSE LLVM_DRIVER_TOOLS)
foreach(tool ${LLVM_DRIVER_TOOLS})
  string(REPLACE "-" "_" tool_entry ${tool})
  # Third field: whether this tool has per-invocation globals and therefore a
  # bound arena plus a lifecycle range to run before its
  # entry point. Tools built without the per-invocation-globals mode have no
  # <tool>_init_lifecycle symbol, so this is decided here rather than by
  # declaring one unconditionally.
  if("${tool}" IN_LIST LLVM_DRIVER_PER_INVOCATION_TOOLS)
    set(tool_per_invocation 1)
  else()
    set(tool_per_invocation 0)
  endif()
  # Fourth field: whether this tool conservatively requires the target and
  # legacy-pass catalogs populated before its invocation context becomes
  # visible to other threads. Only audited lightweight closures receive zero.
  if("${tool}" IN_LIST LLVM_DRIVER_PROCESS_INITIALIZATION_TOOLS)
    set(tool_process_initialization 1)
  else()
    set(tool_process_initialization 0)
  endif()
  get_property(tool_aliases GLOBAL PROPERTY LLVM_DRIVER_TOOL_ALIASES_${tool})
  foreach(alias ${tool_aliases})
    set_property(GLOBAL APPEND PROPERTY LLVM_DRIVER_TOOL_SYMLINKS ${alias})
    string(REPLACE "llvm-" "" alias ${alias})
    string(APPEND def_decl
      "LLVM_DRIVER_TOOL(\"${alias}\", ${tool_entry}, "
      "${tool_per_invocation}, ${tool_process_initialization})\n")
  endforeach()
  get_property(hidden_tool_aliases GLOBAL PROPERTY LLVM_DRIVER_HIDDEN_TOOL_ALIASES_${tool})
  foreach(alias ${hidden_tool_aliases})
    string(REPLACE "llvm-" "" alias ${alias})
    string(APPEND def_decl
      "LLVM_DRIVER_TOOL(\"${alias}\", ${tool_entry}, "
      "${tool_per_invocation}, ${tool_process_initialization})\n")
  endforeach()
endforeach()

set(driver_has_process_initialization 0)
if(LLVM_DRIVER_PROCESS_INITIALIZATION_TOOLS)
  set(driver_has_process_initialization 1)
endif()
set(driver_process_initialization_defines)
foreach(driver_process_capability IN ITEMS
    TARGET_INFOS
    TARGET_MCS
    TARGET_MCAS
    ASM_PARSERS
    TARGETS
    ASM_PRINTERS
    DISASSEMBLERS
    CORE_PASSES
    CODEGEN_PASSES
    SCALAR_PASSES
    IPO_PASSES
    VECTORIZE_PASSES
    TRANSFORM_UTILS_PASSES)
  set(driver_has_process_capability 0)
  if(driver_process_capability IN_LIST
     LLVM_DRIVER_PROCESS_INITIALIZATION_CAPABILITIES)
    set(driver_has_process_capability 1)
  endif()
  string(APPEND driver_process_initialization_defines
    "#ifndef LLVM_DRIVER_HAS_${driver_process_capability}\n"
    "#define LLVM_DRIVER_HAS_${driver_process_capability} "
    "${driver_has_process_capability}\n"
    "#endif\n")
endforeach()
file(WRITE
  "${CMAKE_CURRENT_BINARY_DIR}/LLVMDriverTools.def"
  "#ifndef LLVM_DRIVER_HAS_PROCESS_INITIALIZATION\n"
  "#define LLVM_DRIVER_HAS_PROCESS_INITIALIZATION ${driver_has_process_initialization}\n"
  "#endif\n"
  "${driver_process_initialization_defines}"
  "${def_decl}#undef LLVM_DRIVER_TOOL\n")

macro(generate_driver_tool_targets)
  get_property(LLVM_DRIVER_TOOL_SYMLINKS GLOBAL PROPERTY LLVM_DRIVER_TOOL_SYMLINKS)
  foreach(name ${LLVM_DRIVER_TOOL_SYMLINKS})
    add_llvm_tool_symlink(${name} llvm-driver ALWAYS_GENERATE)
    # Always generate install targets
    llvm_install_symlink(LLVM ${name} llvm-driver ALWAYS_GENERATE)
  endforeach()
endmacro()
