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
#   "aot"     - clang-cl: /dyndbg:aot (AOT; produces .alt.obj)
#   "hybrid"  - clang-cl: /dyndbg:hybrid (on-demand; embedded bitcode)
#
# For "aot" and "hybrid", CheckCXXCompilerFlag verifies the driver accepts the flag
# at configure time (fails fast if the toolchain does not implement it yet).
#
# Set via: cmake -DDYNDBG_MODE=msvc ...

set(DYNDBG_MODE "none" CACHE STRING "Dynamic debugging mode: none, msvc, aot, hybrid, dynamic")
set_property(CACHE DYNDBG_MODE PROPERTY STRINGS none msvc aot hybrid dynamic)

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

# ── Mode: msvc (MSVC /dynamicdeopt -- MSVC's own flag name) ───────────────────
elseif(DYNDBG_MODE STREQUAL "msvc")
    if(NOT IS_MSVC)
        message(FATAL_ERROR "[dyndbg] Mode 'msvc' requires the MSVC compiler (cl.exe), not ${CMAKE_CXX_COMPILER_ID}")
    endif()
    message(STATUS "[dyndbg] Mode: msvc (cl.exe /dynamicdeopt)")
    target_compile_options(demo PRIVATE /dynamicdeopt)
    target_link_options(demo PRIVATE /DYNAMICDEOPT)

# ── Mode: aot (clang-cl /dyndbg:aot) ──────────────────────────────────────────
elseif(DYNDBG_MODE STREQUAL "aot")
    if(NOT IS_CLANG_CL)
        message(FATAL_ERROR "[dyndbg] Mode 'aot' requires clang-cl, not ${CMAKE_CXX_COMPILER_ID}")
    endif()
    check_cxx_compiler_flag("/dyndbg:aot" CXX_COMPILER_SUPPORTS_DYNDBG_AOT)
    if(NOT CXX_COMPILER_SUPPORTS_DYNDBG_AOT)
        message(FATAL_ERROR
            "[dyndbg] This C++ compiler does not accept /dyndbg:aot. "
            "LLVM dynamic-debugging (AOT) is not available in this toolchain yet.")
    endif()
    message(STATUS "[dyndbg] Mode: aot (clang-cl /dyndbg:aot)")
    target_compile_options(demo PRIVATE /dyndbg:aot)

# ── Mode: hybrid (clang-cl /dyndbg:hybrid) ────────────────────────────────────
elseif(DYNDBG_MODE STREQUAL "hybrid")
    if(NOT IS_CLANG_CL)
        message(FATAL_ERROR "[dyndbg] Mode 'hybrid' requires clang-cl, not ${CMAKE_CXX_COMPILER_ID}")
    endif()
    check_cxx_compiler_flag("/dyndbg:hybrid" CXX_COMPILER_SUPPORTS_DYNDBG_HYBRID)
    if(NOT CXX_COMPILER_SUPPORTS_DYNDBG_HYBRID)
        message(FATAL_ERROR
            "[dyndbg] This C++ compiler does not accept /dyndbg:hybrid. "
            "LLVM dynamic-debugging (hybrid) is not available in this toolchain yet.")
    endif()
    message(STATUS "[dyndbg] Mode: hybrid (clang-cl /dyndbg:hybrid)")
    target_compile_options(demo PRIVATE /dyndbg:hybrid)

# ── Mode: dynamic (clang-cl /dyndbg:dynamic) ──────────────────────────────────
elseif(DYNDBG_MODE STREQUAL "dynamic")
    if(NOT IS_CLANG_CL)
        message(FATAL_ERROR "[dyndbg] Mode 'dynamic' requires clang-cl, not ${CMAKE_CXX_COMPILER_ID}")
    endif()
    check_cxx_compiler_flag("/dyndbg:dynamic" CXX_COMPILER_SUPPORTS_DYNDBG_DYNAMIC)
    if(NOT CXX_COMPILER_SUPPORTS_DYNDBG_DYNAMIC)
        message(FATAL_ERROR
            "[dyndbg] This C++ compiler does not accept /dyndbg:dynamic. "
            "LLVM dynamic-debugging (dynamic) is not available in this toolchain yet.")
    endif()
    message(STATUS "[dyndbg] Mode: dynamic (clang-cl /dyndbg:dynamic)")
    target_compile_options(demo PRIVATE /dyndbg:dynamic)

else()
    message(FATAL_ERROR "[dyndbg] Unknown DYNDBG_MODE: ${DYNDBG_MODE}. Use: none, msvc, aot, hybrid, dynamic")
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
# - For DYNDBG_MODE=aot|hybrid, /dyndbg:* is already validated at configure time
#   via CheckCXXCompilerFlag (configure fails if the driver rejects the flag).

enable_testing()

add_test(NAME demo-runs
    COMMAND demo
)
set_tests_properties(demo-runs PROPERTIES
    PASS_REGULAR_EXPRESSION "Final: 495"
)
