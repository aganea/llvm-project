# Dynamic Debugging Demo

Test harness for `/dynamicdeopt` support in Clang/LLVM (COFF/CodeView).
Compares MSVC's implementation with planned AOT and hybrid Clang modes.

## Structure

```
demo/dynamic-debugging/
  CMakeLists.txt       DYNDBG_MODE; CheckCXXCompilerFlag for /dynamicdeopt:aot|:hybrid
  build.ps1            Runs CMake per configuration; skips Clang AOT/hybrid until implemented
  src/
    math_utils.h       Inline function (clamp), function declaration
    math_utils.cpp     Static function, static global, exported function
    main.cpp             Entry point, calls compute() in a loop
  build/               Output directory (created by build.ps1)
    msvc-baseline/     cl.exe /O2 /Z7 (reference, no dynamic debugging)
    msvc-dynamicdeopt/ cl.exe /O2 /Z7 /dynamicdeopt (MSVC reference)
    clang-aot/         (optional) DYNDBG_MODE=aot — skipped by build.ps1 until LLVM implements it
    clang-hybrid/      (optional) DYNDBG_MODE=hybrid — same
```

### CMake modes (`-DDYNDBG_MODE=…`)

| Mode     | Compiler | Compile flags              | Link        | Configure-time check        |
|----------|----------|----------------------------|------------|-----------------------------|
| `none`   | any      | (baseline)                 | `/DEBUG:FULL` | —                        |
| `msvc`   | `cl.exe` | `/dynamicdeopt`            | `/DYNAMICDEOPT` | —                     |
| `aot`    | clang-cl | `/dynamicdeopt:aot`        | `/DYNAMICDEOPT` | `CheckCXXCompilerFlag`  |
| `hybrid` | clang-cl | `/dynamicdeopt:hybrid`     | `/DYNAMICDEOPT` | `CheckCXXCompilerFlag`  |

If `aot`/`hybrid` flags are not accepted, **CMake configure fails** with a clear `FATAL_ERROR`.

### build.ps1

Default **`all`** builds only **`msvc-baseline`** and **`msvc-dynamicdeopt`**.  
**`clang-aot`** / **`clang-hybrid`** are **skipped** with a warning until LLVM implements the flags. Toggle `$ClangDynamicDebuggingImplemented = $true` in `build.ps1` when ready.

## Quick start

```powershell
# Default: both MSVC configs (no Clang — not implemented yet)
.\build.ps1

# MSVC /dynamicdeopt only
.\build.ps1 -Configs msvc-dynamicdeopt

# Clang configs: skipped until implemented (see warning)
.\build.ps1 -Configs clang-aot,clang-hybrid -ClangCl C:\path\to\clang-cl.exe

# Clean and rebuild
.\build.ps1 -Clean

# Manual CMake (will fail configure until Clang accepts /dynamicdeopt:aot)
cmake -S . -B build/manual-aot -G Ninja -DDYNDBG_MODE=aot -DCMAKE_CXX_COMPILER=clang-cl
```

## Tests (CTest)

- **`demo-runs`** — runs `demo`; expects **`Final: 495`** in output.
- **AOT / hybrid** — toolchain support for **`/dynamicdeopt:aot`** / **`/dynamicdeopt:hybrid`** is checked at **configure** time via **`CheckCXXCompilerFlag`** (not a separate CTest).

## What the demo source exercises

- `multiply()` -- `static` function, may be inlined and deleted by optimizer
- `g_call_count` -- `static` global, TU-local state
- `clamp()` -- `inline` function in header, inlined into multiple TUs
- `compute()` -- exported function that calls static + inline functions
- `main()` -- loop calling `compute()`, verifies runtime correctness

## Expected output (all configs)

```
Step 1: result = 3
Step 2: result = 11
Step 3: result = 27
Step 4: result = 53
Step 5: result = 91
Step 6: result = 143
Step 7: result = 211
Step 8: result = 297
Step 9: result = 403
Step 10: result = 495
Final: 495
```
