@echo off
setlocal

set LLVM=c:\git\llvm-project\stage1\bin
set CLANG_CL=%LLVM%\clang-cl.exe
set LLD_LINK=%LLVM%\lld-link.exe
set LLVM_DYNDBG=%LLVM%\llvm-dyndbg.exe
set LLVM_READOBJ=%LLVM%\llvm-readobj.exe

echo =====================================================================
echo  /dyndbg demo - three modes: dynamic, hybrid, aot
echo =====================================================================
echo.

rem ── Mode 1: /dyndbg:dynamic (prep + hotpatch only) ──────────────────
echo --- Mode: dynamic (prep + hotpatch, source recompile at debug time) ---
%CLANG_CL% /Z7 /O2 /dyndbg:dynamic hello.cpp /c /Fohello.dynamic.obj
%LLD_LINK% /DEBUG hello.dynamic.obj /OUT:hello.dynamic.exe
echo.

rem ── Mode 2: /dyndbg:hybrid (prep + embedded bitcode) ────────────────
echo --- Mode: hybrid (prep + embedded bitcode, codegen at debug time) ---
%CLANG_CL% /Z7 /O2 /dyndbg:hybrid hello.cpp /c /Fohello.hybrid.obj
%LLD_LINK% /DEBUG hello.hybrid.obj /OUT:hello.hybrid.exe
echo.

rem ── Mode 3: /dyndbg:aot (prep + ahead-of-time .alt.obj) ─────────────
echo --- Mode: aot (prep + ahead-of-time unoptimized .alt.obj) ---
%CLANG_CL% /Z7 /O2 /dyndbg:aot hello.cpp /c /Fohello.aot.obj
%LLD_LINK% /DEBUG hello.aot.obj /OUT:hello.aot.exe
echo.

rem ── Compare artifacts ───────────────────────────────────────────────
echo =====================================================================
echo  Artifact comparison
echo =====================================================================
echo.

echo --- dynamic mode (no extra artifacts) ---
if exist hello.dynamic.obj (
    echo   hello.dynamic.obj
    for %%F in (hello.dynamic.obj) do echo     size: %%~zF bytes
)
echo.

echo --- hybrid mode (.dyndbg section with embedded bitcode) ---
if exist hello.hybrid.obj (
    echo   hello.hybrid.obj
    for %%F in (hello.hybrid.obj) do echo     size: %%~zF bytes
    echo   Sections:
    %LLVM_READOBJ% --sections hello.hybrid.obj 2>nul | findstr /i ".dyndbg .text"
)
echo.

echo --- aot mode (.alt.obj with unoptimized code) ---
if exist hello.aot.obj (
    echo   hello.aot.obj
    for %%F in (hello.aot.obj) do echo     size: %%~zF bytes
)
if exist hello.alt.obj (
    echo   hello.alt.obj  ^(unoptimized, ahead-of-time^)
    for %%F in (hello.alt.obj) do echo     size: %%~zF bytes
)
echo.

rem ── Verify all produce correct output ───────────────────────────────
echo =====================================================================
echo  Runtime verification (all modes should return 42)
echo =====================================================================
echo.
for %%M in (dynamic hybrid aot) do (
    echo --- %%M ---
    hello.%%M.exe
    echo   exit code: %ERRORLEVEL%
    echo.
)

rem ── Show PDB build info (works for all modes) ──────────────────────
echo =====================================================================
echo  PDB build info (LF_BUILDINFO from hybrid mode)
echo =====================================================================
if exist hello.hybrid.pdb (
    %LLVM_DYNDBG% hello.hybrid.pdb --module hello 2>nul
)

endlocal
