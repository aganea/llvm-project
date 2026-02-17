# We need to execute this script at installation time because the
# DESTDIR environment variable may be unset at configuration time.
# See PR8397.

# Set to an arbitrary directory to silence GNUInstallDirs warnings
# regarding being unable to determine libdir.  This script runs at
# install time (cmake -P) where no project() or enable_language() has
# been called, so CMAKE_SYSTEM_NAME and CMAKE_SIZEOF_VOID_P are unset.
# Since CMake 4.1, GNUInstallDirs warns when no target architecture is
# known; setting these two variables suppresses that AUTHOR_WARNING.
if(NOT DEFINED CMAKE_SYSTEM_NAME)
  set(CMAKE_SYSTEM_NAME "${CMAKE_HOST_SYSTEM_NAME}")
endif()
if(NOT DEFINED CMAKE_SIZEOF_VOID_P)
  # Default to 8 (64-bit); the actual value doesn't matter here because
  # CMAKE_INSTALL_LIBDIR is explicitly set to "lib" below.
  set(CMAKE_SIZEOF_VOID_P 8)
endif()
set(CMAKE_INSTALL_LIBDIR "lib")
include(GNUInstallDirs)

function(install_symlink name target outdir link_or_copy)
  # link_or_copy is the cmake -E command to use for creating the alias.
  # It should be one of: create_symlink, create_hardlink, or copy.

  set(DESTDIR $ENV{DESTDIR})
  if(NOT IS_ABSOLUTE "${outdir}")
    set(outdir "${CMAKE_INSTALL_PREFIX}/${outdir}")
  endif()
  set(outdir "${DESTDIR}${outdir}")

  message(STATUS "Creating ${name}")

  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E ${link_or_copy} "${target}" "${name}"
    WORKING_DIRECTORY "${outdir}"
    RESULT_VARIABLE _result)

  # If hard linking failed (e.g. the install target is on FAT32/exFAT or a
  # network share), fall back to a plain copy so the alias is still created.
  if(NOT _result EQUAL 0 AND "${link_or_copy}" STREQUAL "create_hardlink")
    message(STATUS "Hard link failed for ${name} -- falling back to copy")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E copy "${target}" "${name}"
      WORKING_DIRECTORY "${outdir}")
  endif()

endfunction()
