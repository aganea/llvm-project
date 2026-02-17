@echo off
REM Bootstrap wrapper for build_llvm_release.ps1
REM Ensures PowerShell 7+ (pwsh) is available, then forwards all arguments.

setlocal

where /q pwsh
if %errorlevel% EQU 0 goto :run

echo PowerShell 7+ (pwsh) is required but was not found in PATH.
where /q winget
if %errorlevel% NEQ 0 (
    echo Please install PowerShell 7 from https://github.com/PowerShell/PowerShell/releases
    exit /b 1
)
echo Installing PowerShell 7 via winget...
winget install --id Microsoft.PowerShell --accept-package-agreements --accept-source-agreements --silent
if %errorlevel% NEQ 0 exit /b 1
set "PATH=%PATH%;%ProgramFiles%\PowerShell\7"
where /q pwsh || (echo pwsh still not found. Please restart your terminal. & exit /b 1)

:run
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_llvm_release.ps1" %*
exit /b %errorlevel%
