cmake_minimum_required(VERSION 3.20)
project(DynamicDebuggingDemo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CheckCXXCompilerFlag)

set(SOURCES
    src/math_utils.cpp
    src/main.cpp
)

add_executable(demo ${SOURCES})
target_include_directories(demo PRIVATE src)

# ── Dynamic debugging mode ───────────────────────────────────────────────────
#
# DYNDBG_MODE controls which variant to build:
#   "none"    - plain /O2 /Z7 baseline (no dynamic debugging)
#   "msvc"    - MSVC cl.exe: /dynamicdeopt + link /DYNAMICDEOPT (.alt artifacts)
#   "aot"     - clang-cl: /dynamicdeopt:aot + link /DYNAMICDEOPT (AOT; LLVM TBD)
#   "hybrid"  - clang-cl: /dynamicdeopt:hybrid + link /DYNAMICDEOPT (on-demand; LLVM TBD)
#
# For "aot" and "hybrid", CheckCXXCompilerFlag verifies the driver accepts the flag
# at configure time (fails fast if the toolchain does not implement it yet).
#
# Set via: cmake -DDYNDBG_MODE=msvc ...

set(DYNDBG_MODE "none" CACHE STRING "Dynamic debugging mode: none, msvc, aot, hybrid")
set_property(CACHE DYNDBG_MODE PROPERTY STRINGS none msvc aot hybrid)

# Detect compiler identity
set(IS_MSVC FALSE)
set(IS_CLANG_CL FALSE)
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(IS_MSVC TRUE)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(IS_CLANG_CL TRUE)
endif()

# Always build optimized with debug info
target_compile_options(demo PRIVATE /O2 /Z7 /EHsc)

# /DEBUG:FULL is needed for all dynamic debugging modes
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /DEBUG:FULL")

# ── Mode: none (baseline) ───────────────────────────────────────────────────
if(DYNDBG_MODE STREQUAL "none")
    message(STATUS "[dyndbg] Mode: none (baseline /O2 /Z7)")

# ── Mode: msvc (MSVC /dynamicdeopt) ─────────────────────────────────────────
elseif(DYNDBG_MODE STREQUAL "msvc")
    if(NOT IS_MSVC)
        message(FATAL_ERROR "[dyndbg] Mode 'msvc' requires the MSVC compiler (cl.exe), not ${CMAKE_CXX_COMPILER_ID}")
    endif()
    message(STATUS "[dyndbg] Mode: msvc (/dynamicdeopt)")
    target_compile_options(demo PRIVATE /dynamicdeopt)
    target_link_options(demo PRIVATE /DYNAMICDEOPT)

# ── Mode: aot (clang-cl /dynamicdeopt:aot) ──────────────────────────────────
elseif(DYNDBG_MODE STREQUAL "aot")
    if(NOT IS_CLANG_CL)
        message(FATAL_ERROR "[dyndbg] Mode 'aot' requires clang-cl, not ${CMAKE_CXX_COMPILER_ID}")
    endif()
    check_cxx_compiler_flag("/dynamicdeopt:aot" CXX_COMPILER_SUPPORTS_DYNDEOPT_AOT)
    if(NOT CXX_COMPILER_SUPPORTS_DYNDEOPT_AOT)
        message(FATAL_ERROR
            "[dyndbg] This C++ compiler does not accept /dynamicdeopt:aot. "
            "LLVM dynamic-debugging (AOT) is not available in this toolchain yet.")
    endif()
    message(STATUS "[dyndbg] Mode: aot (clang-cl /dynamicdeopt:aot + /DYNAMICDEOPT) — flag accepted by compiler")
    target_compile_options(demo PRIVATE /dynamicdeopt:aot)
    target_link_options(demo PRIVATE /DYNAMICDEOPT)

# ── Mode: hybrid (clang-cl /dynamicdeopt:hybrid) ──────────────────────────────
elseif(DYNDBG_MODE STREQUAL "hybrid")
    if(NOT IS_CLANG_CL)
        message(FATAL_ERROR "[dyndbg] Mode 'hybrid' requires clang-cl, not ${CMAKE_CXX_COMPILER_ID}")
    endif()
    check_cxx_compiler_flag("/dynamicdeopt:hybrid" CXX_COMPILER_SUPPORTS_DYNDEOPT_HYBRID)
    if(NOT CXX_COMPILER_SUPPORTS_DYNDEOPT_HYBRID)
        message(FATAL_ERROR
            "[dyndbg] This C++ compiler does not accept /dynamicdeopt:hybrid. "
            "LLVM dynamic-debugging (hybrid) is not available in this toolchain yet.")
    endif()
    message(STATUS "[dyndbg] Mode: hybrid (clang-cl /dynamicdeopt:hybrid + /DYNAMICDEOPT) — flag accepted by compiler")
    target_compile_options(demo PRIVATE /dynamicdeopt:hybrid)
    target_link_options(demo PRIVATE /DYNAMICDEOPT)

else()
    message(FATAL_ERROR "[dyndbg] Unknown DYNDBG_MODE: ${DYNDBG_MODE}. Use: none, msvc, aot, hybrid")
endif()

# ── Post-build: report artifacts ─────────────────────────────────────────────

add_custom_command(TARGET demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "--- Build artifacts ---"
    COMMAND ${CMAKE_COMMAND} -E echo "exe: $<TARGET_FILE:demo>"
    VERBATIM
)

# ── CTest ────────────────────────────────────────────────────────────────────
#
# - demo-runs: runs the executable; expects "Final: 495".
# - For DYNDBG_MODE=aot|hybrid, /dynamicdeopt:* is already validated at configure time
#   via CheckCXXCompilerFlag (configure fails if the driver rejects the flag).

enable_testing()

add_test(NAME demo-runs
    COMMAND demo
)
set_tests_properties(demo-runs PROPERTIES
    PASS_REGULAR_EXPRESSION "Final: 495"
)
