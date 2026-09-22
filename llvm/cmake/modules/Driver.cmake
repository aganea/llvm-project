include_guard(GLOBAL)

# Make the semantic lists consumed by the driver-only Clang options real build
# inputs.  Merely putting a list path in target_compile_options() is not
# sufficient: Clang does not currently report special-case-list files in its
# depfile, so Ninja would otherwise keep objects compiled against stale list
# contents.
#
# OBJECT_DEPENDS gives Makefile and Ninja generators the desired timestamp
# dependency.  Follow embedded object libraries as well, since their objects
# bypass the containing archive's own compilation rules.  Do not encode a list
# hash in a preprocessor definition: folded tools reuse component-library PCHs,
# and giving only the object target a new definition makes clang-cl diagnose a
# PCH command-line mismatch.
function(llvm_add_driver_producer_list_dependencies target)
  if(NOT TARGET ${target})
    message(FATAL_ERROR
      "cannot attach llvm-driver producer-list dependencies to missing target "
      "${target}")
  endif()

  get_property(driver_dependencies_attached TARGET ${target} PROPERTY
    LLVM_DRIVER_PRODUCER_LIST_DEPENDENCIES_ATTACHED)
  if(driver_dependencies_attached)
    return()
  endif()
  set_property(TARGET ${target} PROPERTY
    LLVM_DRIVER_PRODUCER_LIST_DEPENDENCIES_ATTACHED TRUE)

  set(driver_producer_lists ${LLVM_DRIVER_STATIC_ARENA_LIST})

  foreach(driver_producer_list ${driver_producer_lists})
    if(NOT IS_ABSOLUTE "${driver_producer_list}" OR
       NOT EXISTS "${driver_producer_list}")
      message(FATAL_ERROR
        "llvm-driver producer list is not an existing absolute path: "
        "${driver_producer_list}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      "${driver_producer_list}")
  endforeach()

  get_property(driver_producer_sources TARGET ${target} PROPERTY SOURCES)
  get_property(driver_producer_source_dir TARGET ${target} PROPERTY SOURCE_DIR)
  foreach(driver_producer_source ${driver_producer_sources})
    if(driver_producer_source MATCHES "^\\$<TARGET_OBJECTS:([^>]+)>$")
      llvm_add_driver_producer_list_dependencies("${CMAKE_MATCH_1}")
      continue()
    endif()
    # Other source generator expressions do not name one object at configure
    # time, so there is no source-file property to attach here.
    if(driver_producer_source MATCHES "^\\$<")
      continue()
    endif()

    if(IS_ABSOLUTE "${driver_producer_source}")
      set(driver_producer_source_path "${driver_producer_source}")
    else()
      cmake_path(ABSOLUTE_PATH driver_producer_source
        BASE_DIRECTORY "${driver_producer_source_dir}" NORMALIZE
        OUTPUT_VARIABLE driver_producer_source_path)
    endif()
    set_property(SOURCE "${driver_producer_source_path}"
      TARGET_DIRECTORY ${target} APPEND PROPERTY OBJECT_DEPENDS
      ${driver_producer_lists})
  endforeach()
endfunction()

# Preserve a tool's executable plugin ABI when it is folded into llvm-driver.
# A folded tool is represented by a custom target, so exporting from it has no
# effect.  Exporting from llvm-driver here would also be too early for the
# selective export-list path: the aggregate's complete (possibly
# driver-private) static-library graph is assembled only after all tool
# projects have been visited.  Record the request for llvm-driver/CMakeLists.txt
# to apply once that graph is final.  Standalone tools retain the ordinary
# AddLLVM behavior.
function(llvm_export_driver_tool_symbols_for_plugins target)
  get_property(driver_tools GLOBAL PROPERTY LLVM_DRIVER_TOOLS)
  if(LLVM_TOOL_LLVM_DRIVER_BUILD AND ${target} IN_LIST driver_tools)
    # On DLL platforms add_llvm_library(PLUGIN_TOOL ...) links against the
    # executable's import library. A folded tool target is only a custom alias,
    # so direct those generic plugin edges to the real aggregate executable.
    set_property(TARGET ${target} PROPERTY
      LLVM_PLUGIN_TOOL_LINK_TARGET llvm-driver)
    if(LLVM_ENABLE_PLUGINS OR LLVM_EXPORT_SYMBOLS_FOR_PLUGINS)
      # Plugin modules may be configured before the aggregate's complete link
      # graph is available. Make the executable linkable now; generation of
      # its selective export list still waits for finalization below.
      set_property(TARGET llvm-driver PROPERTY ENABLE_EXPORTS 1)
      set_property(GLOBAL PROPERTY
        LLVM_DRIVER_EXPORT_SYMBOLS_FOR_PLUGINS TRUE)
    endif()
    return()
  endif()
  export_executable_symbols_for_plugins(${target})
endfunction()

# Selective executable exports use llvm-nm and llvm-readobj to inspect the
# linked archives.  In a folded build those tool targets are custom aliases of
# llvm-driver, so using them to generate llvm-driver's export list would form a
# dependency cycle.  A bootstrap compiler normally provides matching host
# tools beside clang; select those before any tool project requests exports.
# One-stage and cross builds can instead provide LLVM_NM/LLVM_READOBJ,
# LLVM_NATIVE_TOOL_DIR, or LLVM_USE_HOST_TOOLS explicitly.
function(llvm_configure_driver_plugin_export_tools)
  if(NOT LLVM_TOOL_LLVM_DRIVER_BUILD OR
     NOT LLVM_EXPORT_SYMBOLS_FOR_PLUGINS)
    return()
  endif()

  get_filename_component(driver_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  foreach(driver_tool_name llvm-nm llvm-readobj)
    if(driver_tool_name STREQUAL "llvm-nm")
      set(driver_tool_setting LLVM_NM)
    else()
      set(driver_tool_setting LLVM_READOBJ)
    endif()
    if(${driver_tool_setting} OR LLVM_USE_HOST_TOOLS)
      continue()
    endif()

    set(driver_tool_in_native_dir
      "${LLVM_NATIVE_TOOL_DIR}/${driver_tool_name}${LLVM_HOST_EXECUTABLE_SUFFIX}")
    if(LLVM_NATIVE_TOOL_DIR AND EXISTS "${driver_tool_in_native_dir}")
      continue()
    endif()

    set(driver_bootstrap_tool
      "${driver_compiler_dir}/${driver_tool_name}${LLVM_HOST_EXECUTABLE_SUFFIX}")
    if(EXISTS "${driver_bootstrap_tool}")
      set(${driver_tool_setting} "${driver_bootstrap_tool}" CACHE STRING
        "Host ${driver_tool_name} executable. Saves building if cross-compiling."
        FORCE)
      continue()
    endif()

    message(FATAL_ERROR
      "LLVM_EXPORT_SYMBOLS_FOR_PLUGINS requires a host ${driver_tool_name} "
      "outside folded llvm-driver to avoid a dependency cycle. Set "
      "${driver_tool_setting}, LLVM_NATIVE_TOOL_DIR, or LLVM_USE_HOST_TOOLS.")
  endforeach()
endfunction()

# Replace only complete CMake target-name tokens inside a link item.  Plain
# string replacement is unsafe here: LLVMTarget is a prefix of
# LLVMTargetParser, and the driver-private target name also contains the
# original target name as a suffix.
function(llvm_replace_driver_link_item_target out_var replaced_var link_item
    old_target new_target)
  if("${old_target}" STREQUAL "")
    message(FATAL_ERROR "cannot replace an empty llvm-driver link target")
  endif()

  set(driver_rewritten_item "")
  set(driver_replaced FALSE)
  set(driver_scan_offset 0)
  string(LENGTH "${link_item}" driver_item_length)
  string(LENGTH "${old_target}" driver_target_length)

  while(driver_scan_offset LESS driver_item_length)
    string(SUBSTRING "${link_item}" ${driver_scan_offset} -1
      driver_link_suffix)
    string(FIND "${driver_link_suffix}" "${old_target}"
      driver_relative_offset)
    if(driver_relative_offset EQUAL -1)
      string(APPEND driver_rewritten_item "${driver_link_suffix}")
      break()
    endif()

    if(driver_relative_offset GREATER 0)
      string(SUBSTRING "${driver_link_suffix}" 0 ${driver_relative_offset}
        driver_link_prefix)
      string(APPEND driver_rewritten_item "${driver_link_prefix}")
    endif()

    math(EXPR driver_match_offset
      "${driver_scan_offset} + ${driver_relative_offset}")
    math(EXPR driver_match_end
      "${driver_match_offset} + ${driver_target_length}")
    set(driver_complete_target TRUE)
    if(driver_match_offset GREATER 0)
      math(EXPR driver_left_offset "${driver_match_offset} - 1")
      string(SUBSTRING "${link_item}" ${driver_left_offset} 1
        driver_left_character)
      if(driver_left_character MATCHES "^[A-Za-z0-9_.+-]$")
        set(driver_complete_target FALSE)
      endif()
    endif()
    if(driver_match_end LESS driver_item_length)
      string(SUBSTRING "${link_item}" ${driver_match_end} 1
        driver_right_character)
      if(driver_right_character MATCHES "^[A-Za-z0-9_.+-]$")
        set(driver_complete_target FALSE)
      endif()
    endif()

    if(driver_complete_target)
      string(APPEND driver_rewritten_item "${new_target}")
      set(driver_replaced TRUE)
    else()
      string(APPEND driver_rewritten_item "${old_target}")
    endif()
    set(driver_scan_offset ${driver_match_end})
  endwhile()

  set(${out_var} "${driver_rewritten_item}" PARENT_SCOPE)
  set(${replaced_var} ${driver_replaced} PARENT_SCOPE)
endfunction()

# Rewrite a list of link items to the driver-private archive graph.  The common
# LINK_ONLY/BUILD_INTERFACE wrappers are handled structurally.  The fallback
# replacement is needed for conditional generator expressions, whose payload
# CMake intentionally keeps opaque at configure time.
function(llvm_rewrite_driver_per_invocation_link_items out_var)
  get_property(driver_libraries GLOBAL PROPERTY
    LLVM_DRIVER_PER_INVOCATION_LIBRARIES)
  set(driver_rewritten_items)

  foreach(driver_link_item ${ARGN})
    set(driver_rewritten_item "${driver_link_item}")
    set(driver_link_target "")
    set(driver_link_wrapper "")
    if(TARGET "${driver_link_item}")
      set(driver_link_target "${driver_link_item}")
    elseif(driver_link_item MATCHES "^\\$<(LINK_ONLY|BUILD_INTERFACE):([^>]+)>$")
      set(driver_link_wrapper "${CMAKE_MATCH_1}")
      set(driver_link_target "${CMAKE_MATCH_2}")
    endif()

    if(driver_link_target AND TARGET "${driver_link_target}")
      get_property(driver_link_replacement TARGET ${driver_link_target}
        PROPERTY LLVM_DRIVER_PER_INVOCATION_TARGET)
      if(driver_link_replacement)
        if(driver_link_wrapper)
          set(driver_rewritten_item
            "$<${driver_link_wrapper}:${driver_link_replacement}>")
        else()
          set(driver_rewritten_item "${driver_link_replacement}")
        endif()
      endif()
    elseif(driver_link_item MATCHES "^\\$<")
      foreach(driver_library ${driver_libraries})
        get_property(driver_link_replacement TARGET ${driver_library}
          PROPERTY LLVM_DRIVER_PER_INVOCATION_TARGET)
        if(driver_link_replacement)
          llvm_replace_driver_link_item_target(
            driver_rewritten_item driver_link_item_replaced
            "${driver_rewritten_item}" "${driver_library}"
            "${driver_link_replacement}")
        endif()
      endforeach()
    endif()

    list(APPEND driver_rewritten_items "${driver_rewritten_item}")
  endforeach()

  set(${out_var} ${driver_rewritten_items} PARENT_SCOPE)
endfunction()

# Reproduce every internal static-library link graph with driver-private
# per-invocation archives. This runs at the end of llvm/tools/CMakeLists.txt,
# after external projects and their target_link_libraries() calls have
# completed; doing it at
# the first folded tool misses clang/lld and other later-added edges.
function(llvm_finalize_driver_per_invocation_libraries)
  get_property(already_finalized GLOBAL PROPERTY
    LLVM_DRIVER_PER_INVOCATION_LIBRARIES_FINALIZED)
  if(already_finalized)
    return()
  endif()

  get_property(driver_libraries GLOBAL PROPERTY
    LLVM_DRIVER_PER_INVOCATION_LIBRARIES)
  foreach(driver_library ${driver_libraries})
    get_property(driver_per_invocation_library TARGET ${driver_library}
      PROPERTY LLVM_DRIVER_PER_INVOCATION_TARGET)
    foreach(prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      get_property(driver_normal_items TARGET ${driver_library} PROPERTY ${prop})
      if(driver_normal_items MATCHES "-NOTFOUND$")
        set(driver_normal_items)
      endif()
      llvm_rewrite_driver_per_invocation_link_items(
        driver_per_invocation_items
        ${driver_normal_items})
      set_property(TARGET ${driver_per_invocation_library} PROPERTY ${prop}
        "${driver_per_invocation_items}")
    endforeach()

    foreach(prop
        LINK_DIRECTORIES
        LINK_OPTIONS
        INTERFACE_LINK_DIRECTORIES
        INTERFACE_LINK_OPTIONS
        INTERFACE_LINK_DEPENDS)
      set_property(TARGET ${driver_per_invocation_library} PROPERTY ${prop}
        "$<TARGET_PROPERTY:${driver_library},${prop}>")
    endforeach()

    # Embedded object libraries bypass the archive link graph.  Reusing a C++
    # object here would also bypass the producer flags, so reject it rather
    # than silently violating the complete-object-closure invariant.  The one
    # in-tree case today is LLVMSupportBlake3 and contains only C/assembly.
    get_property(driver_embedded_objects TARGET ${driver_per_invocation_library}
      PROPERTY LLVM_DRIVER_EMBEDDED_OBJECT_LIBRARIES)
    foreach(driver_embedded_object ${driver_embedded_objects})
      if(NOT TARGET ${driver_embedded_object})
        message(FATAL_ERROR
          "Per-invocation llvm-driver library ${driver_library} embeds "
          "missing "
          "object target ${driver_embedded_object}")
      endif()
      get_property(driver_embedded_sources TARGET ${driver_embedded_object}
        PROPERTY SOURCES)
      foreach(driver_embedded_source ${driver_embedded_sources})
        if(driver_embedded_source MATCHES "\\.(cc|cpp|cxx|C)(\\.in)?$")
          message(FATAL_ERROR
            "Per-invocation llvm-driver library ${driver_library} embeds C++ "
            "object "
            "target ${driver_embedded_object}; add a driver-private object "
            "copy so the complete object closure receives the producer flags")
        endif()
      endforeach()
    endforeach()
  endforeach()

  set_property(GLOBAL PROPERTY
    LLVM_DRIVER_PER_INVOCATION_LIBRARIES_FINALIZED TRUE)
endfunction()

# Extract target names from direct link items and the generator-expression
# forms used by CMake's static-library interfaces.  Conditional expressions
# are conservatively scanned for every known driver-private library target.
function(llvm_get_driver_link_item_targets out_var)
  get_property(driver_libraries GLOBAL PROPERTY
    LLVM_DRIVER_PER_INVOCATION_LIBRARIES)
  set(driver_targets)
  foreach(driver_link_item ${ARGN})
    set(driver_link_target "")
    if(TARGET "${driver_link_item}")
      set(driver_link_target "${driver_link_item}")
    elseif(driver_link_item MATCHES
           "^\\$<(LINK_ONLY|BUILD_INTERFACE):([^>]+)>$")
      # CMake expands every argument to if() before evaluating MATCHES, so a
      # TARGET "${CMAKE_MATCH_2}" clause in this same condition observes the
      # previous match.  Save and test the new capture in a nested command.
      set(driver_wrapped_target "${CMAKE_MATCH_2}")
      if(TARGET "${driver_wrapped_target}")
        set(driver_link_target "${driver_wrapped_target}")
      endif()
    endif()

    if(driver_link_target)
      list(APPEND driver_targets "${driver_link_target}")
      continue()
    endif()

    if(driver_link_item MATCHES "^\\$<")
      foreach(driver_library ${driver_libraries})
        get_property(driver_per_invocation_library TARGET ${driver_library}
          PROPERTY LLVM_DRIVER_PER_INVOCATION_TARGET)
        llvm_replace_driver_link_item_target(
          driver_ignored_item driver_has_per_invocation_target
          "${driver_link_item}" "${driver_per_invocation_library}"
          "${driver_per_invocation_library}")
        llvm_replace_driver_link_item_target(
          driver_ignored_item driver_has_normal_target
          "${driver_link_item}" "${driver_library}" "${driver_library}")
        if(driver_has_per_invocation_target)
          list(APPEND driver_targets "${driver_per_invocation_library}")
        elseif(driver_has_normal_target)
          list(APPEND driver_targets "${driver_library}")
        endif()
      endforeach()
    endif()
  endforeach()
  list(REMOVE_DUPLICATES driver_targets)
  set(${out_var} ${driver_targets} PARENT_SCOPE)
endfunction()

# Resolve aliases and driver-private copies to the one normal target whose link
# interface is authoritative for closure and lifecycle ordering.
function(llvm_normalize_driver_per_invocation_target out_var target)
  if(NOT TARGET ${target})
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()

  get_property(driver_aliased_target TARGET ${target} PROPERTY ALIASED_TARGET)
  if(driver_aliased_target)
    set(target ${driver_aliased_target})
  endif()
  get_property(driver_original_target TARGET ${target}
    PROPERTY LLVM_DRIVER_PER_INVOCATION_ORIGINAL)
  if(driver_original_target)
    set(target ${driver_original_target})
  endif()
  set(${out_var} ${target} PARENT_SCOPE)
endfunction()

# Visit one target after its dependencies.  The postorder is reversed by the
# caller to obtain one-pass archive order (users before dependencies).  A
# breadth-first walk is not sufficient here: for A -> D and B -> C -> D it can
# produce A,B,D,C, whose reverse initializes C before D.
function(llvm_visit_driver_per_invocation_target token target)
  llvm_normalize_driver_per_invocation_target(driver_target ${target})
  if(NOT driver_target)
    return()
  endif()

  string(MD5 driver_target_key "${driver_target}")
  set(driver_state_property
    "LLVM_DRIVER_PER_INVOCATION_DFS_${token}_${driver_target_key}")
  get_property(driver_visit_state GLOBAL PROPERTY ${driver_state_property})
  if(driver_visit_state STREQUAL "DONE")
    return()
  endif()
  if(driver_visit_state STREQUAL "VISITING")
    message(FATAL_ERROR
      "llvm-driver per-invocation target graph contains a cycle at "
      "${driver_target}; no dependency-first lifecycle order exists")
  endif()
  set_property(GLOBAL PROPERTY ${driver_state_property} VISITING)
  set_property(GLOBAL APPEND PROPERTY
    LLVM_DRIVER_PER_INVOCATION_DFS_KEYS_${token} ${driver_target_key})

  get_property(driver_target_imported TARGET ${driver_target} PROPERTY IMPORTED)
  if(NOT driver_target_imported)
    get_property(driver_target_type TARGET ${driver_target} PROPERTY TYPE)
    if(driver_target_type STREQUAL "SHARED_LIBRARY" OR
       driver_target_type STREQUAL "MODULE_LIBRARY")
      message(FATAL_ERROR
        "llvm-driver per-invocation closure reaches non-static target "
        "${driver_target}; one-image arena ownership is required")
    endif()

    set(driver_target_link_items)
    foreach(driver_link_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      get_property(driver_property_items TARGET ${driver_target}
        PROPERTY ${driver_link_property})
      if(NOT driver_property_items MATCHES "-NOTFOUND$")
        list(APPEND driver_target_link_items ${driver_property_items})
      endif()
    endforeach()
    llvm_get_driver_link_item_targets(driver_child_targets
      ${driver_target_link_items})
    # DFS postorder reverses independent siblings. Visit in reverse so the
    # final archive order preserves their declared link-interface order.
    list(REVERSE driver_child_targets)
    foreach(driver_child_target ${driver_child_targets})
      llvm_visit_driver_per_invocation_target(
        ${token} ${driver_child_target})
    endforeach()

    if(driver_target_type STREQUAL "STATIC_LIBRARY")
      get_property(driver_per_invocation_library TARGET ${driver_target}
        PROPERTY LLVM_DRIVER_PER_INVOCATION_TARGET)
      if(NOT driver_per_invocation_library)
        message(FATAL_ERROR
          "llvm-driver reaches internal static target ${driver_target} without "
          "a driver-private per-invocation copy")
      endif()
      set_property(GLOBAL APPEND PROPERTY
        LLVM_DRIVER_PER_INVOCATION_DFS_POSTORDER_${token} ${driver_target})
    endif()
  endif()

  set_property(GLOBAL PROPERTY ${driver_state_property} DONE)
endfunction()

# Return all driver-private static libraries reachable from the supplied link
# roots, in deterministic one-pass archive order (users before dependencies).
# This walks the actual target interfaces, not LLVM_LINK_COMPONENTS metadata,
# and therefore sees manually/later added clang, lld and MLIR edges too.
function(llvm_collect_driver_per_invocation_library_closure out_var)
  llvm_get_driver_link_item_targets(driver_root_targets ${ARGN})

  get_property(driver_dfs_token GLOBAL PROPERTY
    LLVM_DRIVER_PER_INVOCATION_DFS_TOKEN)
  if(NOT driver_dfs_token)
    set(driver_dfs_token 0)
  endif()
  math(EXPR driver_dfs_token "${driver_dfs_token} + 1")
  set_property(GLOBAL PROPERTY LLVM_DRIVER_PER_INVOCATION_DFS_TOKEN
    ${driver_dfs_token})

  # As with child edges, reverse the visit order so reversing the completed
  # postorder preserves the caller's root order.
  list(REVERSE driver_root_targets)
  foreach(driver_root_target ${driver_root_targets})
    llvm_visit_driver_per_invocation_target(
      ${driver_dfs_token} ${driver_root_target})
  endforeach()

  get_property(driver_library_closure GLOBAL PROPERTY
    LLVM_DRIVER_PER_INVOCATION_DFS_POSTORDER_${driver_dfs_token})
  if(driver_library_closure MATCHES "-NOTFOUND$")
    set(driver_library_closure)
  endif()
  list(REVERSE driver_library_closure)

  # Discard per-walk state. Cycle detection above doubles as a configure-time
  # validation that reversing this closure is a valid dependency-first
  # lifecycle order.
  get_property(driver_dfs_keys GLOBAL PROPERTY
    LLVM_DRIVER_PER_INVOCATION_DFS_KEYS_${driver_dfs_token})
  foreach(driver_target_key ${driver_dfs_keys})
    set_property(GLOBAL PROPERTY
      LLVM_DRIVER_PER_INVOCATION_DFS_${driver_dfs_token}_${driver_target_key})
  endforeach()
  set_property(GLOBAL PROPERTY
    LLVM_DRIVER_PER_INVOCATION_DFS_KEYS_${driver_dfs_token})
  set_property(GLOBAL PROPERTY
    LLVM_DRIVER_PER_INVOCATION_DFS_POSTORDER_${driver_dfs_token})

  set(${out_var} ${driver_library_closure} PARENT_SCOPE)
endfunction()

# Build an internal, driver-only copy of a static LLVM archive.  The normal
# archive remains available to standalone executables, whose globals must keep
# their ordinary eager initialization.
function(llvm_configure_driver_per_invocation_library name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "STATIC" "SOURCES;DEPENDS")
  if(NOT LLVM_DRIVER_PER_INVOCATION_GLOBALS OR NOT ARG_STATIC)
    return()
  endif()

  set(driver_per_invocation_target "LLVMDriverPerInvocation_${name}")
  string(MAKE_C_IDENTIFIER "${name}" driver_lifecycle_section_key)
  set(driver_lifecycle_section "llvmi_lib_${driver_lifecycle_section_key}")
  add_library(${driver_per_invocation_target} STATIC EXCLUDE_FROM_ALL
    ${ARG_SOURCES})

  # Object-backed libraries compile their ordinary sources in obj.<name>; the
  # archive target is only a wrapper and deliberately lacks flags such as the
  # LLVM RTTI/EH policy.  Mirror the target that actually compiles the sources.
  set(driver_per_invocation_compile_target ${name})
  if(TARGET "obj.${name}")
    set(driver_per_invocation_compile_target "obj.${name}")
  endif()

  # Read compile properties through generator expressions so flags added by
  # the library's CMakeLists after add_llvm_library() are preserved by the
  # driver-private copy.
  foreach(prop
      COMPILE_DEFINITIONS
      COMPILE_FEATURES
      COMPILE_OPTIONS
      INCLUDE_DIRECTORIES
      SYSTEM_INCLUDE_DIRECTORIES)
    set_property(TARGET ${driver_per_invocation_target} PROPERTY ${prop}
      "$<TARGET_PROPERTY:${driver_per_invocation_compile_target},${prop}>")
  endforeach()
  # Producer PCHs were built without -fstatic-arena and must not be reused.

  target_compile_options(${driver_per_invocation_target} PRIVATE
    "SHELL:$<TARGET_PROPERTY:${driver_per_invocation_compile_target},COMPILE_FLAGS>"
    "SHELL:-Xclang -fstatic-arena=${driver_lifecycle_section}"
    "SHELL:-Xclang \"-fstatic-arena-list=${LLVM_DRIVER_STATIC_ARENA_LIST}\"")
  set_property(TARGET ${driver_per_invocation_target} PROPERTY
    LLVM_DRIVER_PER_INVOCATION_PRODUCER TRUE)
  llvm_add_driver_producer_list_dependencies(${driver_per_invocation_target})

  set(driver_per_invocation_depends ${ARG_DEPENDS})
  list(REMOVE_DUPLICATES driver_per_invocation_depends)
  if(driver_per_invocation_depends)
    add_dependencies(${driver_per_invocation_target}
      ${driver_per_invocation_depends})
  endif()

  set_target_properties(${driver_per_invocation_target} PROPERTIES
    FOLDER "LLVM Driver/Per-invocation component libraries")
  set_property(TARGET ${driver_per_invocation_target} PROPERTY
    LLVM_DRIVER_LIFECYCLE_SECTION "${driver_lifecycle_section}")
  set_property(TARGET ${driver_per_invocation_target} PROPERTY
    LLVM_DRIVER_PER_INVOCATION_ORIGINAL ${name})
  set_property(TARGET ${name} PROPERTY LLVM_DRIVER_PER_INVOCATION_TARGET
    ${driver_per_invocation_target})

  foreach(driver_per_invocation_file ${ARG_SOURCES})
    if(driver_per_invocation_file MATCHES "^\\$<TARGET_OBJECTS:([^>]+)>$")
      set_property(TARGET ${driver_per_invocation_target} APPEND PROPERTY
        LLVM_DRIVER_EMBEDDED_OBJECT_LIBRARIES "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  set_property(GLOBAL APPEND PROPERTY
    LLVM_DRIVER_PER_INVOCATION_LIBRARIES ${name})
endfunction()

# Materialize one declarative range record per lifecycle section beside a
# tool's generated runner. The C++ template includes that data under separate
# COFF, ELF/Wasm, and execution macros, keeping platform source code out of
# CMake.
# This helper is deliberately callable again at the end of configuration,
# when the complete actual target closure is known: the second configure pass
# atomically replaces the preliminary component-only range data.
function(llvm_configure_driver_lifecycle_runner tool_name output_file)
  set(driver_lifecycle_sections ${ARGN})
  list(REMOVE_DUPLICATES driver_lifecycle_sections)

  if(NOT "${tool_name}" MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
    message(FATAL_ERROR
      "llvm-driver lifecycle tool name is not a C++ identifier: "
      "${tool_name}")
  endif()

  set(driver_template_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
  set(driver_range_template
    "${driver_template_dir}/llvm-driver-initsec-range.def.in")
  set(driver_ranges_template
    "${driver_template_dir}/llvm-driver-initsec-ranges.def.in")
  set(driver_initsec_template
    "${driver_template_dir}/llvm-driver-initsec.cpp.in")
  foreach(driver_template
      "${driver_range_template}"
      "${driver_ranges_template}"
      "${driver_initsec_template}")
    if(NOT EXISTS "${driver_template}")
      message(FATAL_ERROR
        "cannot find llvm-driver lifecycle template: ${driver_template}")
    endif()
  endforeach()

  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${driver_range_template}")
  file(READ "${driver_range_template}" driver_range_template_contents)
  set(LIFECYCLE_RANGE_RECORDS "")
  set(driver_range_id 0)
  foreach(driver_section IN LISTS driver_lifecycle_sections)
    if(NOT "${driver_section}" MATCHES "^llvmi_([A-Za-z0-9_]+)$")
      message(FATAL_ERROR
        "llvm-driver lifecycle section is not a valid identifier: "
        "${driver_section}")
    endif()

    set(LIFECYCLE_RANGE_ID ${driver_range_id})
    set(LIFECYCLE_SECTION "${driver_section}")
    set(LIFECYCLE_SECTION_KEY "${CMAKE_MATCH_1}")
    string(CONFIGURE "${driver_range_template_contents}"
      driver_range_record @ONLY)
    string(APPEND LIFECYCLE_RANGE_RECORDS "${driver_range_record}")
    math(EXPR driver_range_id "${driver_range_id} + 1")
  endforeach()

  set(driver_ranges_file "${output_file}.def")
  configure_file("${driver_ranges_template}" "${driver_ranges_file}" @ONLY)
  get_filename_component(LIFECYCLE_RANGES_INCLUDE
    "${driver_ranges_file}" NAME)
  set(TOOL_NAME "${tool_name}")
  configure_file("${driver_initsec_template}" "${output_file}" @ONLY)
endfunction()

# Finalize the per-tool graph after every tool project has had a chance to add
# link edges.  Besides rewriting normal archives, this is the only
# authoritative source for each tool's lifecycle ranges and dispatch bit.
function(llvm_finalize_driver_per_invocation_graph driver_target)
  if(NOT LLVM_DRIVER_PER_INVOCATION_GLOBALS)
    return()
  endif()

  get_property(driver_graph_already_finalized GLOBAL PROPERTY
    LLVM_DRIVER_PER_INVOCATION_GRAPH_FINALIZED)
  if(driver_graph_already_finalized)
    message(FATAL_ERROR
      "llvm-driver per-invocation graph was finalized more than once")
  endif()

  llvm_finalize_driver_per_invocation_libraries()
  get_property(driver_tools GLOBAL PROPERTY LLVM_DRIVER_TOOLS)
  set(driver_final_per_invocation_tools)
  set(driver_process_initialization_tools)
  set(driver_process_initialization_capabilities)
  set(driver_runner_library_closure)

  # Target and legacy-pass registration is process-wide and must finish before
  # target-using invocations are allowed to overlap.  Keep the provider groups
  # separate: a restricted distribution can, for example, link every target's
  # Info/Desc/AsmParser library without linking any CodeGen, MCA, printer or
  # disassembler implementation.  InitializeAll* is safe only when its whole
  # configured provider group is present in the final image.
  llvm_map_components_to_libnames(driver_process_target_info_libraries
    AllTargetsInfos)
  llvm_map_components_to_libnames(driver_process_target_mc_libraries
    AllTargetsDescs)
  llvm_map_components_to_libnames(driver_process_target_mca_libraries
    AllTargetsMCAs)
  llvm_map_components_to_libnames(driver_process_asm_parser_libraries
    AllTargetsAsmParsers)
  llvm_map_components_to_libnames(driver_process_target_codegen_libraries
    AllTargetsCodeGens)
  llvm_map_components_to_libnames(driver_process_disassembler_libraries
    AllTargetsDisassemblers)

  # Asm-printer implementations normally live in the target CodeGen archive;
  # retain support for a target which splits them into a dedicated component.
  # Match the same source predicate used to generate AsmPrinters.def.
  set(driver_process_asm_printer_libraries)
  foreach(driver_llvm_target ${LLVM_TARGETS_TO_BUILD})
    file(GLOB driver_target_asm_printer_sources
      "${LLVM_MAIN_SRC_DIR}/lib/Target/${driver_llvm_target}/*AsmPrinter.cpp")
    if(NOT driver_target_asm_printer_sources)
      continue()
    endif()
    if(TARGET LLVM${driver_llvm_target}AsmPrinter)
      list(APPEND driver_process_asm_printer_libraries
        LLVM${driver_llvm_target}AsmPrinter)
    elseif(TARGET LLVM${driver_llvm_target}CodeGen)
      list(APPEND driver_process_asm_printer_libraries
        LLVM${driver_llvm_target}CodeGen)
    elseif(TARGET LLVM${driver_llvm_target})
      list(APPEND driver_process_asm_printer_libraries
        LLVM${driver_llvm_target})
    else()
      message(FATAL_ERROR
        "configured LLVM target ${driver_llvm_target} has an asm printer but "
        "no provider library")
    endif()
  endforeach()

  # InitializeAllTargets also initializes every TargetInfo.
  set(driver_process_target_libraries
    ${driver_process_target_codegen_libraries}
    ${driver_process_target_info_libraries})
  # Unlike InitializeAllTargets(), the legacy-pass initialization entry points
  # are independently linkable.  Keep one capability per provider so a
  # minimal folded image containing LLVMCore does not also have to link every
  # CodeGen and transform library.
  llvm_map_components_to_libnames(driver_process_core_pass_libraries Core)
  llvm_map_components_to_libnames(driver_process_codegen_pass_libraries
    CodeGen)
  llvm_map_components_to_libnames(driver_process_scalar_pass_libraries
    ScalarOpts)
  llvm_map_components_to_libnames(driver_process_ipo_pass_libraries IPO)
  llvm_map_components_to_libnames(driver_process_vectorize_pass_libraries
    Vectorize)
  llvm_map_components_to_libnames(driver_process_transform_utils_pass_libraries
    TransformUtils)
  set(driver_process_legacy_pass_libraries
    ${driver_process_core_pass_libraries}
    ${driver_process_codegen_pass_libraries}
    ${driver_process_scalar_pass_libraries}
    ${driver_process_ipo_pass_libraries}
    ${driver_process_vectorize_pass_libraries}
    ${driver_process_transform_utils_pass_libraries})

  set(driver_process_initialization_provider_libraries
    ${driver_process_target_info_libraries}
    ${driver_process_target_mc_libraries}
    ${driver_process_target_mca_libraries}
    ${driver_process_asm_parser_libraries}
    ${driver_process_target_libraries}
    ${driver_process_asm_printer_libraries}
    ${driver_process_disassembler_libraries}
    ${driver_process_legacy_pass_libraries})
  list(REMOVE_DUPLICATES driver_process_initialization_provider_libraries)

  # Libraries which consult TargetRegistry are consumers even when their own
  # archive is not a registration provider.  Classifying them conservatively
  # keeps the bit-0 set limited to utilities whose final closure is known not
  # to observe a catalog while another invocation may be populating it.
  set(driver_process_initialization_consumer_libraries
    LLVMAsmPrinter
    LLVMBitWriter
    LLVMCodeGen
    LLVMDebugInfoDWARF
    LLVMDebugInfoLogicalView
    LLVMDWARFCFIChecker
    LLVMDWARFLinker
    LLVMDWARFLinkerClassic
    LLVMDWARFLinkerParallel
    LLVMExecutionEngine
    LLVMInterpreter
    LLVMJITLink
    LLVMMC
    LLVMMCDisassembler
    LLVMMCJIT
    LLVMObject
    LLVMOrcDebugging
    LLVMOrcJIT
    LLVMOrcShared
    LLVMOrcTargetProcess
    LLVMPasses
    LLVMRuntimeDyld
    LLVMFrontendOpenMP
    LLVMLTO
    LLVMTarget
    LLVMTextAPI
    LLVMipo)

  # New folded tools default to the safe side of the process-initialization
  # boundary. Only these audited utilities may avoid first-use target/pass
  # setup, and even they lose that exemption automatically if their finalized
  # closure acquires a known catalog provider or consumer. This makes the
  # consumer list a check on a tiny allowlist rather than the safety boundary
  # for every tool added to llvm-driver in the future.
  set(driver_process_initialization_lightweight_tools
    llvm-cxxfilt
    llvm-remarkutil)

  # Rewrite every aggregate-driver edge before inspecting it so normal archives
  # cannot win extraction ahead of their driver-private copy.  Do not infer the
  # image-shared closure from these edges: the Clang and LLD helpers deliberately
  # duplicate each tool's private libraries on both its object target and the
  # aggregate driver.  True image-shared roots must be registered explicitly in
  # LLVM_DRIVER_PER_INVOCATION_SHARED_LINK_ROOTS.
  foreach(driver_link_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_property(driver_link_items TARGET ${driver_target}
      PROPERTY ${driver_link_property})
    if(driver_link_items MATCHES "-NOTFOUND$")
      set(driver_link_items)
    endif()
    llvm_rewrite_driver_per_invocation_link_items(driver_rewritten_link_items
      ${driver_link_items})
    set_property(TARGET ${driver_target} PROPERTY ${driver_link_property}
      "${driver_rewritten_link_items}")
  endforeach()

  get_property(driver_link_items TARGET ${driver_target}
    PROPERTY LINK_LIBRARIES)
  get_property(driver_interface_items TARGET ${driver_target}
    PROPERTY INTERFACE_LINK_LIBRARIES)
  llvm_get_driver_link_item_targets(driver_image_link_roots
    ${driver_link_items} ${driver_interface_items})
  get_property(driver_direct_link_items TARGET ${driver_target}
    PROPERTY LLVM_DRIVER_PER_INVOCATION_SHARED_LINK_ROOTS)
  if(driver_direct_link_items MATCHES "-NOTFOUND$")
    set(driver_direct_link_items)
  endif()
  foreach(driver_direct_link_item ${driver_direct_link_items})
    if(NOT TARGET ${driver_direct_link_item})
      message(FATAL_ERROR
        "llvm-driver shared per-invocation root is not a target: "
        "${driver_direct_link_item}")
    endif()
  endforeach()
  llvm_get_driver_link_item_targets(driver_direct_link_roots
    ${driver_direct_link_items})
  foreach(driver_direct_link_root ${driver_direct_link_roots})
    if(NOT driver_direct_link_root IN_LIST driver_image_link_roots)
      message(FATAL_ERROR
        "registered llvm-driver shared root ${driver_direct_link_root} is not "
        "linked directly to ${driver_target}")
    endif()
  endforeach()
  llvm_collect_driver_per_invocation_library_closure(
    driver_direct_library_closure ${driver_direct_link_roots})
  set_property(GLOBAL PROPERTY LLVM_DRIVER_DIRECT_LIBRARY_CLOSURE
    "${driver_direct_library_closure}")
  set(driver_attributed_library_closure ${driver_direct_library_closure})

  foreach(driver_tool ${driver_tools})
    if(NOT TARGET obj.${driver_tool})
      message(FATAL_ERROR
        "folded llvm-driver tool ${driver_tool} has no object target")
    endif()

    # Link helpers in Clang/LLD add dependencies after generate_llvm_objects().
    # Rewrite both the build and usage interfaces now that those edges exist.
    foreach(driver_link_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      get_property(driver_tool_items TARGET obj.${driver_tool}
        PROPERTY ${driver_link_property})
      if(driver_tool_items MATCHES "-NOTFOUND$")
        set(driver_tool_items)
      endif()
      llvm_rewrite_driver_per_invocation_link_items(driver_rewritten_tool_items
        ${driver_tool_items})
      set_property(TARGET obj.${driver_tool} PROPERTY ${driver_link_property}
        "${driver_rewritten_tool_items}")
    endforeach()

    get_property(driver_tool_link_items TARGET obj.${driver_tool}
      PROPERTY LINK_LIBRARIES)
    get_property(driver_tool_interface_items TARGET obj.${driver_tool}
      PROPERTY INTERFACE_LINK_LIBRARIES)
    llvm_collect_driver_per_invocation_library_closure(
      driver_tool_local_closure
      ${driver_tool_link_items} ${driver_tool_interface_items})

    # Image-shared roots are deliberately absent from a folded tool's normal
    # link interface. A tool which actually executes one may declare that
    # association explicitly so process-registry classification sees the
    # root's closure without conservatively charging it to every tool in the
    # aggregate image.
    get_property(driver_tool_process_initialization_shared_roots
      TARGET obj.${driver_tool} PROPERTY
        LLVM_DRIVER_PROCESS_INITIALIZATION_SHARED_LINK_ROOTS)
    if(driver_tool_process_initialization_shared_roots MATCHES "-NOTFOUND$")
      set(driver_tool_process_initialization_shared_roots)
    endif()
    foreach(driver_tool_process_initialization_shared_root
        ${driver_tool_process_initialization_shared_roots})
      if(NOT driver_tool_process_initialization_shared_root IN_LIST
          driver_direct_link_roots)
        message(FATAL_ERROR
          "llvm-driver tool ${driver_tool} declares non-shared process "
          "initialization root "
          "${driver_tool_process_initialization_shared_root}")
      endif()
    endforeach()
    llvm_collect_driver_per_invocation_library_closure(
      driver_tool_process_initialization_shared_closure
      ${driver_tool_process_initialization_shared_roots})
    set(driver_tool_process_initialization_closure
      ${driver_tool_local_closure}
      ${driver_tool_process_initialization_shared_closure})
    list(REMOVE_DUPLICATES driver_tool_process_initialization_closure)

    # Run process catalog setup outside this tool's invocation context. New
    # tools require it by default; only the audited lightweight allowlist can
    # opt out. Deliberately inspect the tool-local closure plus only its
    # explicitly associated shared roots: an image-shared service such as
    # Debuginfod does not make an otherwise lightweight utility a target user.
    set(driver_tool_requires_process_initialization TRUE)
    if(driver_tool IN_LIST driver_process_initialization_lightweight_tools)
      set(driver_tool_requires_process_initialization FALSE)
      foreach(driver_tool_library
          ${driver_tool_process_initialization_closure})
        if(driver_tool_library IN_LIST
             driver_process_initialization_provider_libraries OR
           driver_tool_library IN_LIST
             driver_process_initialization_consumer_libraries)
          set(driver_tool_requires_process_initialization TRUE)
          break()
        endif()
      endforeach()
    endif()
    if(driver_tool_requires_process_initialization)
      list(APPEND driver_process_initialization_tools ${driver_tool})
    endif()

    list(APPEND driver_attributed_library_closure
      ${driver_tool_local_closure})
    # Direct driver roots are image-shared.  Merge their authoritative closure
    # with this tool's own closure so their producer sections participate in
    # both this tool's lifecycle runner and its dispatch bit.
    set(driver_tool_closure
      ${driver_tool_local_closure} ${driver_direct_library_closure})
    list(REMOVE_DUPLICATES driver_tool_closure)
    set_property(GLOBAL PROPERTY
      LLVM_DRIVER_LIBRARY_CLOSURE_${driver_tool} "${driver_tool_closure}")

    # Keep this explicit subset check even though the roots above construct it:
    # it is the fail-closed invariant that prevents a future closure refactor
    # from silently dropping image-shared lifecycle sections from one tool.
    foreach(driver_direct_library ${driver_direct_library_closure})
      if(NOT driver_direct_library IN_LIST driver_tool_closure)
        message(FATAL_ERROR
          "llvm-driver tool ${driver_tool} does not include shared library "
          "${driver_direct_library} in its per-invocation closure")
      endif()
    endforeach()

    get_property(driver_tool_object_is_per_invocation TARGET obj.${driver_tool}
      PROPERTY LLVM_DRIVER_PER_INVOCATION_PRODUCER)
    set(driver_tool_is_per_invocation ${driver_tool_object_is_per_invocation})
    foreach(driver_library ${driver_tool_closure})
      get_property(driver_per_invocation_library TARGET ${driver_library}
        PROPERTY LLVM_DRIVER_PER_INVOCATION_TARGET)
      get_property(driver_library_is_per_invocation
        TARGET ${driver_per_invocation_library}
        PROPERTY LLVM_DRIVER_PER_INVOCATION_PRODUCER)
      if(driver_library_is_per_invocation)
        set(driver_tool_is_per_invocation TRUE)
      endif()
    endforeach()

    if(driver_tool_is_per_invocation)
      # Dependencies initialize before their users, and the tool-private range
      # runs last.  Every library range in the actual closure is included: this
      # includes image-shared direct driver edges, remains correct for OptTable
      # tools, and makes later-added cl:: state safe without a second manual
      # tool classification.
      # Order each rooted closure independently before merging it.  In
      # particular, a library already present as a tool dependency must not
      # move ahead of a direct driver library that depends on it merely because
      # the shared root is appended to the final link line.
      set(driver_init_direct_closure ${driver_direct_library_closure})
      list(REVERSE driver_init_direct_closure)
      set(driver_init_tool_closure ${driver_tool_local_closure})
      list(REVERSE driver_init_tool_closure)
      set(driver_init_library_closure
        ${driver_init_direct_closure} ${driver_init_tool_closure})
      list(REMOVE_DUPLICATES driver_init_library_closure)
      set(driver_lifecycle_sections)
      foreach(driver_library ${driver_init_library_closure})
        get_property(driver_per_invocation_library TARGET ${driver_library}
          PROPERTY LLVM_DRIVER_PER_INVOCATION_TARGET)
        get_property(driver_library_section
          TARGET ${driver_per_invocation_library}
          PROPERTY LLVM_DRIVER_LIFECYCLE_SECTION)
        if(NOT driver_library_section)
          message(FATAL_ERROR
            "per-invocation library ${driver_library} has no lifecycle section")
        endif()
        list(APPEND driver_lifecycle_sections ${driver_library_section})
      endforeach()
      if(driver_tool_object_is_per_invocation)
        get_property(driver_tool_section TARGET obj.${driver_tool}
          PROPERTY LLVM_DRIVER_LIFECYCLE_SECTION)
        if(NOT driver_tool_section)
          message(FATAL_ERROR
            "per-invocation tool object ${driver_tool} has no lifecycle section")
        endif()
        list(APPEND driver_lifecycle_sections ${driver_tool_section})
      endif()
      list(REMOVE_DUPLICATES driver_lifecycle_sections)

      foreach(driver_direct_library ${driver_direct_library_closure})
        get_property(driver_per_invocation_library
          TARGET ${driver_direct_library}
          PROPERTY LLVM_DRIVER_PER_INVOCATION_TARGET)
        get_property(driver_direct_section
          TARGET ${driver_per_invocation_library}
          PROPERTY LLVM_DRIVER_LIFECYCLE_SECTION)
        if(NOT driver_direct_section OR
           NOT driver_direct_section IN_LIST driver_lifecycle_sections)
          message(FATAL_ERROR
            "llvm-driver tool ${driver_tool} lifecycle runner omits shared "
            "section ${driver_direct_section} from ${driver_direct_library}")
        endif()
      endforeach()

      get_property(driver_initsec_file TARGET obj.${driver_tool}
        PROPERTY LLVM_DRIVER_LIFECYCLE_RUNNER)
      get_property(driver_initsec_name TARGET obj.${driver_tool}
        PROPERTY LLVM_DRIVER_LIFECYCLE_TOOL_NAME)
      if(NOT driver_initsec_file OR NOT driver_initsec_name)
        message(FATAL_ERROR
          "per-invocation tool ${driver_tool} has no lifecycle runner")
      endif()
      llvm_configure_driver_lifecycle_runner(
        ${driver_initsec_name} ${driver_initsec_file}
        ${driver_lifecycle_sections})

      # Validate the materialized range data, not just the list handed to the
      # generator. This fails closed if a future template/helper change drops,
      # duplicates, or reorders one of the authoritative closure sections.
      set(driver_initsec_ranges_file "${driver_initsec_file}.def")
      if(NOT EXISTS "${driver_initsec_ranges_file}")
        message(FATAL_ERROR
          "llvm-driver tool ${driver_tool} did not materialize lifecycle "
          "ranges at ${driver_initsec_ranges_file}")
      endif()
      file(READ "${driver_initsec_ranges_file}"
        driver_materialized_range_contents)
      string(REGEX MATCHALL "llvmi_[A-Za-z0-9_]+"
        driver_materialized_lifecycle_sections
        "${driver_materialized_range_contents}")
      if(NOT "${driver_materialized_lifecycle_sections}" STREQUAL
             "${driver_lifecycle_sections}")
        message(FATAL_ERROR
          "llvm-driver tool ${driver_tool} materialized lifecycle ranges "
          "that differ from its final link closure:\n"
          "  expected: ${driver_lifecycle_sections}\n"
          "  actual:   ${driver_materialized_lifecycle_sections}")
      endif()
      set_property(TARGET obj.${driver_tool} PROPERTY
        LLVM_DRIVER_FINAL_LIFECYCLE_SECTIONS
        "${driver_lifecycle_sections}")
      list(APPEND driver_runner_library_closure
        ${driver_init_library_closure})
      list(APPEND driver_final_per_invocation_tools ${driver_tool})
    endif()
  endforeach()

  list(REMOVE_DUPLICATES driver_final_per_invocation_tools)
  set_property(GLOBAL PROPERTY LLVM_DRIVER_PER_INVOCATION_TOOLS
    "${driver_final_per_invocation_tools}")
  list(REMOVE_DUPLICATES driver_process_initialization_tools)
  set_property(GLOBAL PROPERTY LLVM_DRIVER_PROCESS_INITIALIZATION_TOOLS
    "${driver_process_initialization_tools}")

  # One final walk is deliberately redundant with the per-tool walks.  It
  # catches a direct llvm-driver dependency that was not attached to an object
  # target and rejects any internal static archive for which no per-invocation
  # copy was made.
  get_property(driver_link_items TARGET ${driver_target}
    PROPERTY LINK_LIBRARIES)
  get_property(driver_interface_items TARGET ${driver_target}
    PROPERTY INTERFACE_LINK_LIBRARIES)
  llvm_collect_driver_per_invocation_library_closure(driver_image_closure
    ${driver_link_items} ${driver_interface_items})
  list(REMOVE_DUPLICATES driver_image_closure)

  # Record complete target catalog groups and each independently linkable pass
  # provider. The generated driver uses these as compile-time guards, so a
  # minimal folded image never gains an undefined reference merely because a
  # different catalog family is present.
  foreach(driver_process_capability IN ITEMS
      target_infos
      target_mcs
      target_mcas
      asm_parsers
      targets
      asm_printers
      disassemblers
      core_passes
      codegen_passes
      scalar_passes
      ipo_passes
      vectorize_passes
      transform_utils_passes)
    if(driver_process_capability STREQUAL "target_infos")
      set(driver_process_capability_libraries
        ${driver_process_target_info_libraries})
    elseif(driver_process_capability STREQUAL "target_mcs")
      set(driver_process_capability_libraries
        ${driver_process_target_mc_libraries})
    elseif(driver_process_capability STREQUAL "target_mcas")
      set(driver_process_capability_libraries
        ${driver_process_target_mca_libraries})
    elseif(driver_process_capability STREQUAL "asm_parsers")
      set(driver_process_capability_libraries
        ${driver_process_asm_parser_libraries})
    elseif(driver_process_capability STREQUAL "targets")
      set(driver_process_capability_libraries
        ${driver_process_target_libraries})
    elseif(driver_process_capability STREQUAL "asm_printers")
      set(driver_process_capability_libraries
        ${driver_process_asm_printer_libraries})
    elseif(driver_process_capability STREQUAL "disassemblers")
      set(driver_process_capability_libraries
        ${driver_process_disassembler_libraries})
    elseif(driver_process_capability STREQUAL "core_passes")
      set(driver_process_capability_libraries
        ${driver_process_core_pass_libraries})
    elseif(driver_process_capability STREQUAL "codegen_passes")
      set(driver_process_capability_libraries
        ${driver_process_codegen_pass_libraries})
    elseif(driver_process_capability STREQUAL "scalar_passes")
      set(driver_process_capability_libraries
        ${driver_process_scalar_pass_libraries})
    elseif(driver_process_capability STREQUAL "ipo_passes")
      set(driver_process_capability_libraries
        ${driver_process_ipo_pass_libraries})
    elseif(driver_process_capability STREQUAL "vectorize_passes")
      set(driver_process_capability_libraries
        ${driver_process_vectorize_pass_libraries})
    elseif(driver_process_capability STREQUAL "transform_utils_passes")
      set(driver_process_capability_libraries
        ${driver_process_transform_utils_pass_libraries})
    endif()

    if(NOT driver_process_capability_libraries)
      continue()
    endif()

    # A TargetInfo-only image has no Target implementations to initialize, so
    # do not classify it as a partial `targets` group merely because that
    # capability also depends on TargetInfo. Once any CodeGen provider is
    # present, however, the complete configured Target+TargetInfo group is
    # required. Other capabilities use their provider list directly.
    if(driver_process_capability STREQUAL "targets")
      set(driver_process_presence_libraries
        ${driver_process_target_codegen_libraries})
    else()
      set(driver_process_presence_libraries
        ${driver_process_capability_libraries})
    endif()

    set(driver_process_has_any_capability_provider FALSE)
    set(driver_process_present_capability_libraries)
    foreach(driver_process_presence_library
        ${driver_process_presence_libraries})
      if(driver_process_presence_library IN_LIST driver_image_closure)
        set(driver_process_has_any_capability_provider TRUE)
        list(APPEND driver_process_present_capability_libraries
          ${driver_process_presence_library})
      endif()
    endforeach()

    set(driver_process_has_complete_capability TRUE)
    set(driver_process_missing_capability_libraries)
    foreach(driver_process_capability_library
        ${driver_process_capability_libraries})
      if(NOT driver_process_capability_library IN_LIST driver_image_closure)
        set(driver_process_has_complete_capability FALSE)
        list(APPEND driver_process_missing_capability_libraries
          ${driver_process_capability_library})
      endif()
    endforeach()

    # The dispatch table has one process-initialization once_flag. Silently
    # omitting a partial InitializeAll* target family would permanently
    # complete that once while leaving a folded tool to perform
    # InitializeNative* inside its invocation context. Reject that
    # configuration instead of permitting a late process-registry mutation
    # when invocations overlap. Pass-provider capabilities each contain one
    # independently linkable archive and therefore cannot be partial.
    if(driver_process_has_any_capability_provider AND
       NOT driver_process_has_complete_capability)
      string(REPLACE ";" ", " driver_process_present_capability_text
        "${driver_process_present_capability_libraries}")
      string(REPLACE ";" ", " driver_process_missing_capability_text
        "${driver_process_missing_capability_libraries}")
      message(FATAL_ERROR
        "LLVM_DRIVER_PER_INVOCATION_GLOBALS=ON cannot safely build "
        "llvm-driver with a partial ${driver_process_capability} process "
        "initialization provider group.\n"
        "  present: ${driver_process_present_capability_text}\n"
        "  missing: ${driver_process_missing_capability_text}\n"
        "Align LLVM_TARGETS_TO_BUILD and the folded tool/library closure so "
        "the image contains either none or all providers in this group.")
    endif()

    if(driver_process_has_complete_capability)
      string(TOUPPER "${driver_process_capability}"
        driver_process_capability_name)
      list(APPEND driver_process_initialization_capabilities
        ${driver_process_capability_name})
    endif()
  endforeach()
  set_property(GLOBAL PROPERTY
    LLVM_DRIVER_PROCESS_INITIALIZATION_CAPABILITIES
    "${driver_process_initialization_capabilities}")

  list(REMOVE_DUPLICATES driver_attributed_library_closure)
  list(REMOVE_DUPLICATES driver_runner_library_closure)
  foreach(driver_image_library ${driver_image_closure})
    if(NOT driver_image_library IN_LIST driver_attributed_library_closure)
      message(FATAL_ERROR
        "llvm-driver image library ${driver_image_library} is neither "
        "attributed to a folded tool nor registered in "
        "LLVM_DRIVER_PER_INVOCATION_SHARED_LINK_ROOTS")
    endif()
    if(NOT driver_image_library IN_LIST driver_runner_library_closure)
      message(FATAL_ERROR
        "llvm-driver image library ${driver_image_library} has no lifecycle "
        "range in any folded tool runner")
    endif()
  endforeach()
  foreach(driver_runner_library ${driver_runner_library_closure})
    if(NOT driver_runner_library IN_LIST driver_image_closure)
      message(FATAL_ERROR
        "llvm-driver lifecycle runners reference library "
        "${driver_runner_library}, which is absent from the final image")
    endif()
  endforeach()

  set_property(GLOBAL PROPERTY
    LLVM_DRIVER_PER_INVOCATION_GRAPH_FINALIZED TRUE)
endfunction()

# Generate a standalone main wrapper for a driver-capable tool and, when the
# current configuration folds it into llvm-driver, configure its object target
# and record it in the multicall image graph.  The generated file list is
# returned explicitly because generate_llvm_objects() is a macro whose caller
# consumes ALL_FILES.
function(llvm_configure_driver_tool out_var name object_target)
  cmake_parse_arguments(PARSE_ARGV 3 ARG ""
    "FOLD_INTO_DRIVER"
    "LLVM_CONFIG_OPTIONS;FILES;DEPENDS;COMMON_DEPENDS;LINK_COMPONENTS")

  string(REPLACE "-" "_" TOOL_NAME ${name})

  set(INITLLVM_ARGS "")

  # When Clang is invoked as an OS utility (e.g., c17), it needs to follow the
  # POSIX specification for how utilities respond to signals.
  if(${name} STREQUAL "clang")
    set(INITLLVM_ARGS ", /*InstallPipeSignalExitHandler=*/true, /*NeedsPOSIXUtilitySignalHandling=*/true")
  endif()

  foreach(path ${CMAKE_MODULE_PATH})
    if(EXISTS ${path}/llvm-driver-template.cpp.in)
      configure_file(
        ${path}/llvm-driver-template.cpp.in
        ${CMAKE_CURRENT_BINARY_DIR}/${name}-driver.cpp)
      break()
    endif()
  endforeach()

  set(driver_files ${ARG_FILES})
  list(APPEND driver_files ${CMAKE_CURRENT_BINARY_DIR}/${name}-driver.cpp)

  if(ARG_FOLD_INTO_DRIVER
     AND LLVM_TOOL_LLVM_DRIVER_BUILD
     AND (NOT LLVM_DISTRIBUTION_COMPONENTS OR
          ${name} IN_LIST LLVM_DISTRIBUTION_COMPONENTS))
    # Per-invocation lowering is gated on this exact condition, not merely on
    # the object library existing: outside it the tool still gets a standalone
    # executable linked from these same objects, and a standalone binary does
    # not bind an arena or walk lifecycle ranges.
    if(LLVM_DRIVER_PER_INVOCATION_GLOBALS)
      set(driver_per_invocation_library_targets)
      set(driver_library_closure)

      # Compile exactly the LLVM libraries in the tool's transitive resolved
      # link closure into driver-only copies. Standalone executables keep their
      # ordinary process-lifetime globals.
      llvm_map_components_to_libnames(driver_direct_libraries
        ${ARG_LINK_COMPONENTS})
      llvm_collect_driver_per_invocation_library_closure(driver_library_closure
        ${driver_direct_libraries})

      foreach(driver_library ${driver_library_closure})
        get_property(driver_per_invocation_library TARGET ${driver_library}
          PROPERTY LLVM_DRIVER_PER_INVOCATION_TARGET)
        list(APPEND driver_per_invocation_library_targets
          ${driver_per_invocation_library})
      endforeach()

      set_property(GLOBAL PROPERTY
        LLVM_DRIVER_LIBRARY_CLOSURE_${name} "${driver_library_closure}")
      set_property(GLOBAL APPEND PROPERTY
        LLVM_DRIVER_LIBRARY_CLOSURE_TOOLS ${name})

      # Move this tool's selected globals into per-invocation storage and send
      # their dynamic initialization to its lifecycle section. TOOL_NAME
      # already has '-' replaced by '_', making it a valid section suffix.
      target_compile_options(${object_target} PRIVATE
        "SHELL:-Xclang -fstatic-arena=llvmi_${TOOL_NAME}"
        "SHELL:-Xclang \"-fstatic-arena-list=${LLVM_DRIVER_STATIC_ARENA_LIST}\"")
      set_property(TARGET ${object_target} PROPERTY
        LLVM_DRIVER_PER_INVOCATION_PRODUCER TRUE)

      set(driver_initsec_file
        ${CMAKE_CURRENT_BINARY_DIR}/${name}-initsec.cpp)
      set_property(TARGET ${object_target} PROPERTY
        LLVM_DRIVER_LIFECYCLE_RUNNER ${driver_initsec_file})
      set_property(TARGET ${object_target} PROPERTY
        LLVM_DRIVER_LIFECYCLE_SECTION "llvmi_${TOOL_NAME}")
      set_property(TARGET ${object_target} PROPERTY
        LLVM_DRIVER_LIFECYCLE_TOOL_NAME "${TOOL_NAME}")

      # Attach via target_sources, not the returned file list: by this point
      # that list contains $<TARGET_OBJECTS:...>, so appending the init runner
      # there would not compile it into the folded tool object. Do not emit a
      # preliminary runner here: Clang/LLD helpers add link edges later. Mark
      # the path generated and let the one authoritative graph finalizer write
      # it after every folded tool project has been visited.
      set_source_files_properties(${driver_initsec_file} PROPERTIES
        GENERATED TRUE)
      target_sources(${object_target} PRIVATE ${driver_initsec_file})
      llvm_add_driver_producer_list_dependencies(${object_target})
      set_property(GLOBAL APPEND PROPERTY
        LLVM_DRIVER_PER_INVOCATION_TOOLS ${name})
    endif()

    # The per-invocation dispatch bit follows the complete resolved closure.
    # The tool object itself is one producer candidate; every driver-private
    # library in its closure is another.
    set(driver_has_per_invocation_globals FALSE)
    get_property(driver_tool_has_per_invocation TARGET ${object_target}
      PROPERTY LLVM_DRIVER_PER_INVOCATION_PRODUCER)
    if(driver_tool_has_per_invocation)
      set(driver_has_per_invocation_globals TRUE)
    endif()
    foreach(driver_per_invocation_library
        ${driver_per_invocation_library_targets})
      get_property(driver_library_has_per_invocation
        TARGET ${driver_per_invocation_library}
        PROPERTY LLVM_DRIVER_PER_INVOCATION_PRODUCER)
      if(driver_library_has_per_invocation)
        set(driver_has_per_invocation_globals TRUE)
      endif()
    endforeach()
    if(driver_has_per_invocation_globals)
      set_property(GLOBAL APPEND PROPERTY
        LLVM_DRIVER_PER_INVOCATION_TOOLS ${name})
    endif()

    set_property(GLOBAL APPEND PROPERTY
      LLVM_DRIVER_COMPONENTS ${ARG_LINK_COMPONENTS})
    set_property(GLOBAL APPEND PROPERTY
      LLVM_DRIVER_DEPS ${ARG_DEPENDS} ${ARG_COMMON_DEPENDS})
    set_property(GLOBAL APPEND PROPERTY LLVM_DRIVER_OBJLIBS "${object_target}")

    set_property(GLOBAL APPEND PROPERTY LLVM_DRIVER_TOOLS ${name})
    set_property(GLOBAL APPEND PROPERTY
      LLVM_DRIVER_TOOL_ALIASES_${name} ${name})
    target_link_libraries(${object_target} PUBLIC ${LLVM_PTHREAD_LIB})
    if(LLVM_DRIVER_PER_INVOCATION_GLOBALS)
      # No normal LLVM archive is allowed on this path: even one late archive
      # extraction could reintroduce an eager copy of a selected global. The
      # Driver-private targets mirror component dependencies and preserve only
      # the original non-LLVM/system link interface.
      target_link_libraries(${object_target} PRIVATE
        ${driver_per_invocation_library_targets})
    else()
      llvm_config(${object_target} ${ARG_LLVM_CONFIG_OPTIONS}
        ${ARG_LINK_COMPONENTS})
    endif()
  endif()

  set(${out_var} ${driver_files} PARENT_SCOPE)
endfunction()
