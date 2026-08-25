#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Script for building the LLVM installer on Windows,
    used for the releases at https://github.com/llvm/llvm-project/releases

.DESCRIPTION
    Builds LLVM release packages for Windows (x86, x64, arm64).
    Performs a 2-stage build with PGO optimization.
    Can bootstrap a fresh VM by installing all prerequisites.

    By default, builds x64 using the local source tree and auto-detected
    Python.  Use -DownloadSource to download a tagged release tarball
    instead.

    Build steps (in order):
      1. libxml2   - Build libxml2 (inside stage0 directory)
      2. stage0    - Build stage 0 with the system compiler
      3. pgo       - PGO instrumented build + training + profile merge
      4. stage1    - Build stage 1 with stage0 clang + PGO profile
      5. package   - Create NSIS installer
      6. tarball   - Generate full install tarball

.PARAMETER Version
    LLVM version string (e.g. "19.1.0").  If omitted, auto-detected from
    the source tree.

.PARAMETER x86
    Build for x86 (32-bit).

.PARAMETER x64
    Build for x64 (64-bit).  This is the default if no architecture is specified.

.PARAMETER arm64
    Build for ARM64 (AArch64).

.PARAMETER DownloadSource
    Download and extract the tagged release source tarball from GitHub
    instead of using the local source tree.  Requires -Version.

.PARAMETER ForceMSVC
    Force using MSVC (cl.exe) instead of clang-cl for the stage 0 build.

.PARAMETER InstallPrerequisites
    Install build prerequisites (Visual Studio, CMake, Python, etc.) via
    winget before building.  When used with -x86 or -arm64, also installs
    the matching Python architecture for LLDB.

.PARAMETER StartAt
    Resume a previous build from a specific step.  Artifacts from earlier
    steps must already exist on disk.  The step being restarted gets a
    clean directory; earlier steps are skipped entirely.

    Valid steps: libxml2, stage0, pgo, stage1, package, tarball

    Pass an empty string (-StartAt "") or "?" (-StartAt "?") to display
    the list of available steps and exit.

.PARAMETER Help
    Display this help message and exit.

.EXAMPLE
    .\build_llvm_release.ps1
    Full x64 build using the local source tree.

.EXAMPLE
    .\build_llvm_release.ps1 -x64 -arm64
    Build for both x64 and ARM64.

.EXAMPLE
    .\build_llvm_release.ps1 -Version 19.1.0 -DownloadSource
    Download version 19.1.0 sources and build x64.

.EXAMPLE
    .\build_llvm_release.ps1 -InstallPrerequisites -x64
    Install prerequisites, then do a full x64 build.

.EXAMPLE
    .\build_llvm_release.ps1 -InstallPrerequisites -x64 -x86 -arm64
    Install prerequisites (including per-arch Python), then build all architectures.

.EXAMPLE
    .\build_llvm_release.ps1 -x64 -StartAt stage1
    Resume from stage1 (reuses stage0 + PGO profile from a previous run).

.EXAMPLE
    .\build_llvm_release.ps1 -x64 -StartAt package
    Re-create just the NSIS installer (stage1 must already be built).

.EXAMPLE
    .\build_llvm_release.ps1 -StartAt ""
    Display the list of available build steps and exit.

.NOTES
    Python is auto-detected from PATH for x64 builds.  For x86 and arm64
    builds, the script probes standard install locations for a matching
    Python architecture.  Use -InstallPrerequisites to install per-arch
    Python automatically.

    Environment variables:
      LLVM_NINJA_OVERRIDE  - Override the ninja binary and optionally provide
                             extra flags.  The first token is the executable,
                             remaining tokens are prepended to every ninja
                             invocation.
                             Example: LLVM_NINJA_OVERRIDE="myninja.exe --flag1 --flag2"
#>

param(
    [string]$Version,
    [switch]$x86,
    [switch]$x64,
    [switch]$arm64,
    [switch]$DownloadSource,
    [switch]$ForceMSVC,
    [switch]$InstallPrerequisites,
    [string]$StartAt,
    [switch]$Help
)

#===============================================================================
# PowerShell 7 self-relaunch
#
# This script requires PowerShell 7+ features (ternary operator, null-
# coalescing, improved error handling, etc.).  If we detect we are running
# under Windows PowerShell 5.x, we attempt to find or install PowerShell 7
# and re-launch ourselves under it, forwarding all original arguments.
#===============================================================================
if ($PSVersionTable.PSVersion.Major -lt 7) {
    # Try to find pwsh in PATH first.
    $pwsh = Get-Command pwsh -ErrorAction SilentlyContinue
    if (-not $pwsh) {
        # Check the default install location.
        $defaultPath = "$env:ProgramFiles\PowerShell\7\pwsh.exe"
        if (Test-Path $defaultPath) {
            $pwsh = Get-Item $defaultPath
        }
    }

    if (-not $pwsh) {
        Write-Host "PowerShell 7 is required but not found." -ForegroundColor Yellow
        Write-Host "Attempting to install via winget..." -ForegroundColor Yellow
        $winget = Get-Command winget -ErrorAction SilentlyContinue
        if (-not $winget) {
            Write-Error "Cannot install PowerShell 7: winget is not available.`nPlease install PowerShell 7 manually from https://aka.ms/powershell-release?tag=stable"
            exit 1
        }
        winget install --id Microsoft.PowerShell --source winget --accept-source-agreements --accept-package-agreements
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Failed to install PowerShell 7 via winget (exit code $LASTEXITCODE)."
            exit 1
        }
        # Refresh PATH so we can find the newly installed pwsh.
        $env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                     [System.Environment]::GetEnvironmentVariable("Path", "User")
        $pwsh = Get-Command pwsh -ErrorAction SilentlyContinue
        if (-not $pwsh) {
            $defaultPath = "$env:ProgramFiles\PowerShell\7\pwsh.exe"
            if (Test-Path $defaultPath) {
                $pwsh = Get-Item $defaultPath
            }
        }
        if (-not $pwsh) {
            Write-Error "PowerShell 7 was installed but pwsh.exe could not be found in PATH."
            exit 1
        }
        Write-Host "PowerShell 7 installed successfully." -ForegroundColor Green
    }

    # Re-launch this script under PowerShell 7, forwarding all arguments.
    # Note: avoid PS7-only syntax (??  ternary, etc.) in this block since
    # it executes under PS5.
    $pwshPath = if ($pwsh.Source) { $pwsh.Source } else { $pwsh.Path }
    Write-Host "Re-launching under PowerShell 7 ($pwshPath)..." -ForegroundColor Cyan
    & $pwshPath -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath @PSBoundParameters
    exit $LASTEXITCODE
}

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Save the console mode so we can restore it at exit.  Child processes
# (cmake, ninja, link.exe, etc.) sometimes disable virtual terminal
# processing and don't restore it, which breaks arrow keys and ESC in
# the parent shell after the script finishes.
$script:SavedConsoleMode = $null
try {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ConsoleMode {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr GetStdHandle(int nStdHandle);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetConsoleMode(IntPtr hConsoleHandle, out uint lpMode);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetConsoleMode(IntPtr hConsoleHandle, uint dwMode);
    const int STD_INPUT_HANDLE = -10;
    public static uint Get() {
        uint mode;
        GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), out mode);
        return mode;
    }
    public static void Set(uint mode) {
        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), mode);
    }
}
'@ -ErrorAction SilentlyContinue
    $script:SavedConsoleMode = [ConsoleMode]::Get()
} catch {
    # Non-fatal; we just won't be able to restore the console mode.
}

# Default to x64 if no architecture specified
if (-not $x86 -and -not $x64 -and -not $arm64) {
    $x64 = $true
}

#===============================================================================
# Step-resume support
#===============================================================================

$script:StepOrder   = @('libxml2','stage0','pgo','stage1','package','tarball')

if ($PSBoundParameters.ContainsKey('StartAt')) {
    if (-not $StartAt -or $StartAt -eq '?') {
        Write-Host "Available -StartAt steps (in order):" -ForegroundColor Cyan
        for ($i = 0; $i -lt $script:StepOrder.Count; $i++) {
            Write-Host "  $($i + 1). $($script:StepOrder[$i])"
        }
        exit 0
    }
    if ($StartAt -notin $script:StepOrder) {
        Write-Error "Invalid -StartAt value: '$StartAt'. Valid steps: $($script:StepOrder -join ', ')"
        exit 1
    }
}

$script:StartAtStep = if ($StartAt) { $StartAt } else { $null }

function Test-ShouldRun {
    <#
    .SYNOPSIS
        Returns $true if the given step should execute (i.e. it is at or after
        the -StartAt step).  When -StartAt is not set, always returns $true.
    #>
    param([Parameter(Mandatory)][string]$Step)
    if (-not $script:StartAtStep) { return $true }
    return $script:StepOrder.IndexOf($Step) -ge $script:StepOrder.IndexOf($script:StartAtStep)
}

function Test-IsStartStep {
    <#
    .SYNOPSIS
        Returns $true if the given step is exactly the -StartAt step (the step
        whose directory should be cleaned before a fresh rebuild).
    #>
    param([Parameter(Mandatory)][string]$Step)
    return ($script:StartAtStep -and $script:StartAtStep -eq $Step)
}

function Assert-PathExists {
    <#
    .SYNOPSIS
        Validates that a path exists on disk.  Used when skipping steps to
        ensure the artifacts from a prior run are still present.
    #>
    param(
        [Parameter(Mandatory)][string]$Path,
        [string]$Description = $Path
    )
    if (-not (Test-Path $Path)) {
        Write-Error "Required artifact missing: $Description`n  Path: $Path`n  Hint: run the earlier build steps first, or use a different -StartAt value."
        exit 1
    }
}

function Remove-StepDirectory {
    <#
    .SYNOPSIS
        Removes a directory (or file) if it exists, printing a message.
        Used to clean a step's build directory before restarting it.
    #>
    param([Parameter(Mandatory)][string]$Path)
    if (Test-Path $Path) {
        Write-SubStep "Cleaning: $Path"
        Remove-Item -Recurse -Force $Path
    }
}

# Ninja override: allow using a custom ninja binary with extra flags.
# Usage: $env:LLVM_NINJA_OVERRIDE = "myninja.exe --flag1 --flag2"
if ($env:LLVM_NINJA_OVERRIDE) {
    $tokens = @($env:LLVM_NINJA_OVERRIDE.Trim() -split '\s+')
    $script:NinjaCommand  = $tokens[0]
    [string[]]$script:NinjaExtraArgs = if ($tokens.Count -gt 1) { $tokens[1..($tokens.Count - 1)] } else { @() }
    Write-Host "Using custom ninja: $($script:NinjaCommand)" -ForegroundColor Cyan
    if ($script:NinjaExtraArgs.Count -gt 0) {
        Write-Host "  Extra ninja args: $($script:NinjaExtraArgs -join ' ')" -ForegroundColor Cyan
    }
} else {
    $script:NinjaCommand  = 'ninja'
    $script:NinjaExtraArgs = @()
}

# Filter out tests that are known to fail.
$env:LIT_FILTER_OUT = "gh110231.cpp|crt_initializers.cpp|init-order-atexit.cpp|use_after_return_linkage.cpp|trace-malloc-unbalanced.test|trace-malloc-2.test|TraceMallocTest"

#===============================================================================
# Utility functions
#===============================================================================

function Write-Step {
    param([string]$Message)
    Write-Host "`n==== $Message ====`n" -ForegroundColor Cyan
}

function Write-SubStep {
    param([string]$Message)
    Write-Host "  -- $Message" -ForegroundColor DarkCyan
}

function Write-CMakeCacheFile {
    <#
    .SYNOPSIS
        Writes CMake -D flags to an initial cache script file to avoid
        exceeding the Windows MAX_PATH / command-line length limit.
    .DESCRIPTION
        Accepts an array of CMake arguments, extracts every -DVAR=VALUE
        entry, writes them as set(VAR VALUE CACHE <type> "") lines in a
        temporary .cmake file, and returns a hashtable with:
          CacheFile  - the path to the generated file
          OtherFlags - any non -D flags that were in the input array
    #>
    param(
        [Parameter(Mandatory)]
        [string[]]$Flags,
        [string]$FileName = 'cache_flags.cmake'
    )
    $cacheLines = @()
    $otherFlags = @()
    foreach ($f in $Flags) {
        if ($f -match '^-D([^:=]+)(?::([^=]*))?=(.*)$') {
            $varName  = $Matches[1]
            $varType  = if ($Matches[2]) { $Matches[2] } else { 'STRING' }
            $varValue = ($Matches[3] -replace '"', '').Replace('\', '/')   # strip quotes, use forward slashes
            $cacheLines += "set($varName `"$varValue`" CACHE $varType `"`" FORCE)"
        } else {
            $otherFlags += $f
        }
    }
    $cachePath = (Join-Path $PWD $FileName).Replace('\', '/')
    $cacheLines -join "`n" | Set-Content -Path $cachePath -Encoding UTF8
    return @{
        CacheFile  = $cachePath
        OtherFlags = $otherFlags
    }
}

function Invoke-NativeCommand {
    <#
    .SYNOPSIS
        Runs a native command and throws on non-zero exit code.
    .NOTES
        This is intentionally a simple function (no [Parameter()] attributes
        and no [CmdletBinding()]) so that PowerShell does NOT inject common
        parameters (-Confirm, -OutVariable, -ErrorAction, etc.).  Those
        common parameters collide with native flags like -C, -O, -E, etc.
    #>
    $Command = $args[0]
    $Arguments = $args[1..($args.Count - 1)]
    Write-Host "+ $Command $($Arguments -join ' ')" -ForegroundColor DarkGray
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Command $($Arguments -join ' ')"
    }
}

function Invoke-WithRetry {
    <#
    .SYNOPSIS
        Runs a command up to $MaxRetries times, returning on first success.
    #>
    param(
        [Parameter(Mandatory)]
        [string]$Command,
        [string[]]$Arguments,
        [int]$MaxRetries = 3
    )
    for ($i = 1; $i -le $MaxRetries; $i++) {
        Write-Host "+ [attempt $i/$MaxRetries] $Command $($Arguments -join ' ')" -ForegroundColor DarkGray
        & $Command @Arguments
        if ($LASTEXITCODE -eq 0) { return }
        if ($i -lt $MaxRetries) {
            Write-Host "  Attempt $i failed (exit code $LASTEXITCODE), retrying..." -ForegroundColor Yellow
        }
    }
    throw "Command failed after $MaxRetries attempts: $Command $($Arguments -join ' ')"
}

function Get-LitRerunCommand {
    <#
    .SYNOPSIS
        Tries to recover the exact lit invocation that a ninja test target
        (e.g. check-llvm) would run, so a retry can re-run lit directly
        with '--filter-failed' instead of re-running the whole suite.
    .DESCRIPTION
        Only simple, single-command lit targets (add_lit_testsuite /
        umbrella_lit_testsuite - e.g. check-llvm, check-clang, check-lld,
        check-clang-tools, check-clangd) reduce to exactly one command via
        'ninja -t commands'. Targets that fan out into a nested build
        (e.g. check-runtimes, which drives a separate ExternalProject
        ninja tree) don't, and are intentionally left alone by returning
        $null so the caller can fall back to full-suite retries.
    #>
    param([Parameter(Mandatory)][string]$Target)

    $output = & $script:NinjaCommand @script:NinjaExtraArgs -t commands $Target 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $output) { return $null }

    $litLines = @($output | Where-Object { $_ -match 'llvm-lit(\.py)?["\s]' })
    if ($litLines.Count -ne 1) { return $null }

    $line = $litLines[0].Trim()
    # Bail if this isn't a single plain invocation (shell chaining means
    # blindly appending flags to the end could land in the wrong place).
    if ($line -match '&&|\|\||\bcmd(\.exe)?\s+/c\b') { return $null }

    return $line
}

function Invoke-TestTarget {
    <#
    .SYNOPSIS
        Runs a single test target with retries. When a retry is needed,
        re-runs only the tests that failed on the previous attempt
        (via lit's own '--filter-failed', which lit tracks for us in
        '.lit_test_times.txt') instead of the whole suite, falling back
        to the old full-suite retry when that isn't possible for this
        target.
    #>
    param(
        [Parameter(Mandatory)]
        [string]$Target,
        [int]$MaxRetries = 3
    )
    $ninjaArgs = @($script:NinjaExtraArgs) + @($Target)
    Write-Host "+ [attempt 1/$MaxRetries] $script:NinjaCommand $($ninjaArgs -join ' ')" -ForegroundColor DarkGray
    & $script:NinjaCommand @ninjaArgs
    if ($LASTEXITCODE -eq 0) { return }

    $rerunCommand = Get-LitRerunCommand -Target $Target
    if (-not $rerunCommand) {
        Write-SubStep "Could not isolate a lit invocation for $Target; falling back to full-suite retries"
        for ($i = 2; $i -le $MaxRetries; $i++) {
            Write-Host "  Attempt $($i - 1) failed, retrying full suite (attempt $i/$MaxRetries)..." -ForegroundColor Yellow
            & $script:NinjaCommand @ninjaArgs
            if ($LASTEXITCODE -eq 0) { return }
        }
        throw "Command failed after $MaxRetries attempts: $script:NinjaCommand $($ninjaArgs -join ' ')"
    }

    for ($i = 2; $i -le $MaxRetries; $i++) {
        Write-Host "  Attempt $($i - 1) failed; re-running only the previously-failed tests (attempt $i/$MaxRetries)..." -ForegroundColor Yellow
        $filteredCommand = "$rerunCommand --filter-failed --allow-empty-runs"
        Write-Host "+ [filtered rerun] $filteredCommand" -ForegroundColor DarkGray
        cmd /c $filteredCommand
        if ($LASTEXITCODE -eq 0) { return }
    }
    throw "Tests for $Target still failing after $MaxRetries attempts"
}

function Get-ForwardSlashPath {
    <#
    .SYNOPSIS
        Convert backslashes to forward slashes (CMake expects this for compiler paths).
    #>
    param([string]$Path)
    return $Path.Replace('\', '/')
}

function Test-FileChecksum {
    <#
    .SYNOPSIS
        Verifies a downloaded file's hash and throws if it doesn't match.
    #>
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Algorithm,
        [Parameter(Mandatory)][string]$ExpectedHash
    )
    $actual = (Get-FileHash -Path $Path -Algorithm $Algorithm).Hash
    if ($actual -ne $ExpectedHash.ToUpperInvariant()) {
        throw "Checksum mismatch for ${Path}:`n  expected $ExpectedHash`n  actual   $actual"
    }
    Write-SubStep "Checksum OK ($Algorithm): $Path"
}

#===============================================================================
# Version detection
#===============================================================================

function Get-LLVMVersionFromSource {
    <#
    .SYNOPSIS
        Extracts LLVM version from cmake/Modules/LLVMVersion.cmake.
    #>
    param([string]$SourceDir)
    $versionFile = Join-Path $SourceDir 'cmake' 'Modules' 'LLVMVersion.cmake'
    if (-not (Test-Path $versionFile)) {
        throw "Cannot find LLVMVersion.cmake at: $versionFile"
    }
    $content = Get-Content $versionFile -Raw
    $major = if ($content -match 'LLVM_VERSION_MAJOR\s+(\d+)') { $Matches[1] } else { throw "Cannot parse LLVM_VERSION_MAJOR" }
    $minor = if ($content -match 'LLVM_VERSION_MINOR\s+(\d+)') { $Matches[1] } else { throw "Cannot parse LLVM_VERSION_MINOR" }
    $patch = if ($content -match 'LLVM_VERSION_PATCH\s+(\d+)') { $Matches[1] } else { throw "Cannot parse LLVM_VERSION_PATCH" }
    $suffix = if ($content -match 'LLVM_VERSION_SUFFIX\s+(\S+)\)') { $Matches[1] } else { '' }
    # Don't include the "git" development suffix in the release version
    if ($suffix -eq 'git') { $suffix = '' }
    return @{
        Major = $major
        Minor = $minor
        Patch = $patch
        Suffix = $suffix
        Full = "${major}.${minor}.${patch}${suffix}"
    }
}

#===============================================================================
# Prerequisite validation
#===============================================================================

function Assert-Prerequisites {
    <#
    .SYNOPSIS
        Validates that all required build tools are available.  Applies PATH
        fixups for tools that install to non-standard locations, then checks
        every required tool.  Aborts with a clear error if anything is missing.
    #>
    Write-Step "Validating prerequisites"

    # Fix up PATH for tools that install to well-known non-PATH locations.
    $pathFixups = @(
        "$env:ProgramFiles\Git\usr\bin"                     # sh, bash, sed, grep (needed by tests)
        "${env:ProgramFiles(x86)}\GnuWin32\bin"             # make (GnuWin32 default)
        "$env:ProgramFiles\GnuWin32\bin"                    # make (alt location)
        "$env:ProgramFiles\CMake\bin"                       # cmake
        "${env:ProgramFiles(x86)}\NSIS"                     # makensis (x86 default)
        "$env:ProgramFiles\NSIS"                            # makensis (alt location)
        "$env:ProgramFiles\7-Zip"                           # 7z
        "${env:ProgramFiles(x86)}\7-Zip"                    # 7z (x86 install)
    )
    foreach ($dir in $pathFixups) {
        if ((Test-Path $dir) -and ($env:PATH -notlike "*$dir*")) {
            $env:PATH = "$env:PATH;$dir"
            Write-SubStep "Added to PATH: $dir"
        }
    }

    # Check all required tools.
    $required = @(
        @{ Name = "CMake";     Cmd = "cmake" }
        @{ Name = "Ninja";     Cmd = "ninja" }
        @{ Name = "Python";    Cmd = "python" }
        @{ Name = "7-Zip";     Cmd = "7z" }
        @{ Name = "NSIS";      Cmd = "makensis" }
        @{ Name = "Git";       Cmd = "git" }
        @{ Name = "SWIG";      Cmd = "swig" }
        @{ Name = "Perl";      Cmd = "perl" }
        @{ Name = "Make";      Cmd = "make" }
    )

    $missing = @()
    foreach ($tool in $required) {
        $cmd = Get-Command $tool.Cmd -ErrorAction SilentlyContinue
        if ($cmd) {
            Write-SubStep "$($tool.Name): $($cmd.Source)"
        } else {
            $missing += $tool.Name
        }
    }

    if ($missing.Count -gt 0) {
        Write-Host ""
        Write-Error ("Missing required tools: $($missing -join ', ')`n" +
            "Run the script with -InstallPrerequisites to install them, or install manually.")
        exit 1
    }

    Write-SubStep "All prerequisites found."
}

#===============================================================================
# Prerequisite installation (for fresh VMs)
#===============================================================================

function Install-Prerequisites {
    Write-Step "Installing prerequisites"

    # Ensure winget is available
    $winget = Get-Command winget -ErrorAction SilentlyContinue
    if (-not $winget) {
        throw "winget is not available. Please install App Installer from the Microsoft Store."
    }

    # Version requirements (from the original build_llvm_release.bat):
    #   - Python 3.13 for all architectures
    #   - SWIG 4.1.1 specifically (required for LLDB)
    #   - NSIS with the strlen_8192 patch (standard NSIS 3.x includes this)
    #   - Perl is needed for the OpenMP run-time
    #   - GNU Make is needed by LLDB API tests
    #   - 7-Zip 20.x or older avoids needing admin for symlink extraction,
    #     but 21.x+ works if running as administrator
    $tools = @(
        @{ Name = "cmake";      WingetId = "Kitware.CMake";           Version = "";       Verify = "cmake" }
        @{ Name = "ninja";      WingetId = "Ninja-build.Ninja";       Version = "";       Verify = "ninja" }
        @{ Name = "python3.13"; WingetId = "Python.Python.3.13";      Version = "3.13";   Verify = "python" }
        @{ Name = "7z";         WingetId = "7zip.7zip";               Version = "";       Verify = "7z" }
        @{ Name = "nsis";       WingetId = "NSIS.NSIS";               Version = "3";      Verify = "makensis" }
        @{ Name = "git";        WingetId = "Git.Git";                 Version = "";       Verify = "git" }
        @{ Name = "swig";       WingetId = "SWIG.SWIG";              Version = "4.1.1";  Verify = "swig" }
        @{ Name = "perl";       WingetId = "StrawberryPerl.StrawberryPerl"; Version = ""; Verify = "perl" }
        @{ Name = "make";       WingetId = "GnuWin32.Make";              Version = "";  Verify = "make" }
    )

    foreach ($tool in $tools) {
        $cmd = Get-Command $tool.Verify -ErrorAction SilentlyContinue
        if ($cmd) {
            Write-SubStep "$($tool.Name) is already installed: $($cmd.Source)"
        } else {
            $versionArg = if ($tool.Version) { @("--version", $tool.Version) } else { @() }
            Write-SubStep "Installing $($tool.Name) ($($tool.WingetId) $($tool.Version))..."
            winget install --id $tool.WingetId @versionArg --accept-package-agreements --accept-source-agreements --silent
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "Failed to install $($tool.Name) via winget. You may need to install it manually."
            }
        }
    }

    # Install per-architecture Python for cross-arch builds (LLDB needs matching Python).
    # The main Python install above covers the host architecture (typically x64).
    $archPython = @()
    if ($x86)   { $archPython += @{ Arch = 'x86';   Suffix = '-32' } }
    if ($arm64) { $archPython += @{ Arch = 'arm64'; Suffix = '-arm64' } }
    foreach ($ap in $archPython) {
        $basePath = "$env:LOCALAPPDATA\Programs\Python"
        $existing = if (Test-Path $basePath) {
            Get-ChildItem -Path $basePath -Directory -Filter "Python3*$($ap.Suffix)" |
                Where-Object { Test-Path (Join-Path $_.FullName 'python.exe') }
        } else { $null }
        if ($existing) {
            Write-SubStep "Python for $($ap.Arch) already installed: $($existing[0].FullName)"
        } else {
            Write-SubStep "Installing Python for $($ap.Arch) (Python.Python.3.13 --architecture $($ap.Arch))..."
            winget install --id Python.Python.3.13 --architecture $($ap.Arch) --accept-package-agreements --accept-source-agreements --silent
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "Failed to install Python for $($ap.Arch). LLDB may not work for this architecture."
            }
        }
    }

    # Visual Studio Build Tools - special handling
    Install-VisualStudio

    # Refresh PATH so newly installed tools are visible.
    # (Assert-Prerequisites will add non-standard locations like GnuWin32, Git usr/bin, etc.)
    $machinePath = [Environment]::GetEnvironmentVariable("PATH", "Machine")
    $userPath = [Environment]::GetEnvironmentVariable("PATH", "User")
    $env:PATH = "$machinePath;$userPath"
}

function Install-VisualStudio {
    # Components required by LLVM:
    #   - VCTools workload: core C++ compiler, linker, libs
    #   - ATL: required by llvm/include/llvm/DebugInfo/PDB/DIA/DIASupport.h (<atlbase.h>)
    #   - MFC: commonly needed alongside ATL
    #   - DIA SDK: PDB debug info reader, checked by cmake/config-ix.cmake (LLVM_ENABLE_DIA_SDK)
    #     The DIA SDK is part of the "Visual Studio C++ core features" component.
    #
    # Component IDs reference:
    #   https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools

    $requiredComponents = @(
        "Microsoft.VisualStudio.Workload.VCTools"
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
        "Microsoft.VisualStudio.Component.VC.Tools.ARM64"
        "Microsoft.VisualStudio.Component.VC.ATL"
        "Microsoft.VisualStudio.Component.VC.ATL.ARM64"
        "Microsoft.VisualStudio.Component.VC.ATLMFC"
        "Microsoft.VisualStudio.Component.VC.ATLMFC.ARM64"
        "Microsoft.VisualStudio.Component.VC.DiagnosticTools"  # includes DIA SDK
        # The latest Windows SDK is included automatically via --includeRecommended
    )

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -nologo -latest -products '*' -format json | ConvertFrom-Json
        if ($vs) {
            Write-SubStep "Visual Studio already installed: $($vs[0].installationPath)"
            Write-SubStep "Ensuring required components are installed..."
            # Use vs_installer to modify the existing installation
            $addArgs = ($requiredComponents | ForEach-Object { "--add $_" }) -join ' '
            $installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vs_installer.exe"
            if (Test-Path $installer) {
                $installPath = $vs[0].installationPath
                & $installer modify --installPath "$installPath" $addArgs --passive --wait 2>&1 | Out-Null
                # vs_installer returns 0 on success, non-zero codes may just mean "already installed"
            }
            return
        }
    }

    Write-SubStep "Installing Visual Studio Build Tools 2022..."
    $addArgs = ($requiredComponents | ForEach-Object { "--add $_" }) -join ' '
    winget install Microsoft.VisualStudio.2022.BuildTools `
        --accept-package-agreements --accept-source-agreements --silent `
        --override "$addArgs --includeRecommended --passive --wait"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install Visual Studio Build Tools."
    }
}

#===============================================================================
# Visual Studio detection
#===============================================================================

function Find-VisualStudio {
    <#
    .SYNOPSIS
        Finds the Visual Studio installation and returns the path to VsDevCmd.bat.
    #>

    if ($env:VSINSTALLDIR) {
        Write-SubStep "Using enabled Visual Studio installation: $env:VSINSTALLDIR"
        $vsInstall = $env:VSINSTALLDIR
    } else {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (-not (Test-Path $vswhere)) {
            throw "Cannot find vswhere.exe. Is Visual Studio installed?"
        }
        $vsInstall = & $vswhere -nologo -latest -products '*' -all -property installationPath
        if (-not $vsInstall) {
            throw "Cannot find any Visual Studio installation."
        }
        Write-SubStep "Detected Visual Studio: $vsInstall"
    }

    $vsDevCmd = Join-Path $vsInstall 'Common7' 'Tools' 'VsDevCmd.bat'
    if (-not (Test-Path $vsDevCmd)) {
        throw "Cannot find VsDevCmd.bat at: $vsDevCmd"
    }
    return $vsDevCmd
}

function Enter-VsDevEnvironment {
    <#
    .SYNOPSIS
        Sources VsDevCmd.bat and imports the resulting environment variables into PowerShell.
    #>
    param(
        [Parameter(Mandatory)]
        [string]$VsDevCmd,
        [Parameter(Mandatory)]
        [string]$Arch
    )
    Write-SubStep "Setting up VS developer environment for $Arch..."
    # Run VsDevCmd.bat in a cmd subprocess and capture the resulting environment
    $envBlock = cmd /c "`"$VsDevCmd`" -arch=$Arch -no_logo && set" 2>&1
    foreach ($line in $envBlock) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
        }
    }
}

#===============================================================================
# Python setup
#===============================================================================

function Find-Python {
    <#
    .SYNOPSIS
        Locates a Python installation for the given target architecture.

        For x64 (amd64) builds, uses the system Python from PATH.
        For cross-arch builds (x86, arm64), probes standard install locations
        for any Python 3.x matching the target architecture.  Falls back to
        the PATH Python with a warning if no per-arch install is found.
    #>
    param(
        [Parameter(Mandatory)][string]$Arch
    )

    $pythonHome = $null

    if ($Arch -eq 'amd64') {
        # Host-arch build: use system Python from PATH.
        $python = Get-Command python -ErrorAction SilentlyContinue
        if (-not $python) {
            throw "Cannot find python in PATH. Run with -InstallPrerequisites or install Python manually."
        }
        $pythonHome = Split-Path $python.Source
    } else {
        # Cross-arch build: probe standard per-arch install locations.
        # Python installs to %LOCALAPPDATA%\Programs\Python\Python3XX[-32|-arm64]
        $suffix = switch ($Arch) {
            'x86'   { '-32' }
            'arm64' { '-arm64' }
        }
        $basePath = "$env:LOCALAPPDATA\Programs\Python"
        if (Test-Path $basePath) {
            $candidates = Get-ChildItem -Path $basePath -Directory -Filter "Python3*$suffix" |
                Where-Object { Test-Path (Join-Path $_.FullName 'python.exe') } |
                Sort-Object Name -Descending
            if ($candidates.Count -gt 0) {
                $pythonHome = $candidates[0].FullName
            }
        }

        if (-not $pythonHome) {
            # Fall back to PATH Python with a warning.
            $python = Get-Command python -ErrorAction SilentlyContinue
            if ($python) {
                $pythonHome = Split-Path $python.Source
                Write-Warning ("No per-arch Python found for $Arch (looked in $basePath\Python3*$suffix).`n" +
                    "  Falling back to PATH Python: $pythonHome`n" +
                    "  LLDB may not work correctly for the $Arch target.`n" +
                    "  Run with -InstallPrerequisites -$Arch to install the correct Python.")
            } else {
                throw ("Cannot find Python for $Arch. No per-arch install in $basePath and no python in PATH.`n" +
                    "Run with -InstallPrerequisites to install Python.")
            }
        } else {
            Invoke-NativeCommand (Join-Path $pythonHome 'python.exe') --version
        }
    }

    $env:PYTHONHOME = $pythonHome
    $env:PATH = "$pythonHome;$env:PATH"
    Write-SubStep "Using Python from: $pythonHome"
    return $pythonHome
}

#===============================================================================
# Build helpers
#===============================================================================

function Build-LibXml2 {
    <#
    .SYNOPSIS
        Builds libxml2 and sets $script:LibXmlInstallDir to the install path.
    .NOTES
        Uses a script-scoped variable instead of return to avoid PowerShell
        capturing native command stdout as part of the function's return value,
        which would break Ninja's \r progress display and corrupt the path.
    #>
    param(
        [Parameter(Mandatory)]
        [string]$SourceDir
    )
    Write-SubStep "Building libxml2..."
    $libxmlBuild = 'libxmlbuild'
    New-Item -ItemType Directory -Path $libxmlBuild -Force | Out-Null
    Push-Location $libxmlBuild
    try {
        $libxmlFlags = @(
            "-DCMAKE_BUILD_TYPE=Release"
            "-DCMAKE_INSTALL_PREFIX=$(Join-Path $PWD 'install')"
            "-DBUILD_SHARED_LIBS=OFF"
            "-DLIBXML2_WITH_C14N=OFF"
            "-DLIBXML2_WITH_CATALOG=OFF"
            "-DLIBXML2_WITH_DEBUG=OFF"
            "-DLIBXML2_WITH_DOCB=OFF"
            "-DLIBXML2_WITH_FTP=OFF"
            "-DLIBXML2_WITH_HTML=OFF"
            "-DLIBXML2_WITH_HTTP=OFF"
            "-DLIBXML2_WITH_ICONV=OFF"
            "-DLIBXML2_WITH_ICU=OFF"
            "-DLIBXML2_WITH_ISO8859X=OFF"
            "-DLIBXML2_WITH_LEGACY=OFF"
            "-DLIBXML2_WITH_LZMA=OFF"
            "-DLIBXML2_WITH_MEM_DEBUG=OFF"
            "-DLIBXML2_WITH_MODULES=OFF"
            "-DLIBXML2_WITH_OUTPUT=ON"
            "-DLIBXML2_WITH_PATTERN=OFF"
            "-DLIBXML2_WITH_PROGRAMS=OFF"
            "-DLIBXML2_WITH_PUSH=OFF"
            "-DLIBXML2_WITH_PYTHON=OFF"
            "-DLIBXML2_WITH_READER=OFF"
            "-DLIBXML2_WITH_REGEXPS=OFF"
            "-DLIBXML2_WITH_RUN_DEBUG=OFF"
            "-DLIBXML2_WITH_SAX1=ON"
            "-DLIBXML2_WITH_SCHEMAS=OFF"
            "-DLIBXML2_WITH_SCHEMATRON=OFF"
            "-DLIBXML2_WITH_TESTS=OFF"
            "-DLIBXML2_WITH_THREADS=ON"
            "-DLIBXML2_WITH_THREAD_ALLOC=OFF"
            "-DLIBXML2_WITH_TREE=ON"
            "-DLIBXML2_WITH_VALID=OFF"
            "-DLIBXML2_WITH_WRITER=OFF"
            "-DLIBXML2_WITH_XINCLUDE=OFF"
            "-DLIBXML2_WITH_XPATH=OFF"
            "-DLIBXML2_WITH_XPTR=OFF"
            "-DLIBXML2_WITH_ZLIB=OFF"
            "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
        )
        $cache = Write-CMakeCacheFile -Flags $libxmlFlags -FileName 'libxml2_cache.cmake'
        $otherFlags = $cache.OtherFlags
        Invoke-NativeCommand cmake -GNinja -C $cache.CacheFile @otherFlags $SourceDir
        Invoke-NativeCommand $script:NinjaCommand @script:NinjaExtraArgs install
        $script:LibXmlInstallDir = Get-ForwardSlashPath (Join-Path $PWD 'install')
    } finally {
        Pop-Location
    }
}

function Build-Zlib {
    <#
    .SYNOPSIS
        Builds zlib and sets $script:ZlibInstallDir to the install path.
    .NOTES
        See Build-LibXml2 for why a script-scoped variable is used instead of return.
    #>
    param(
        [Parameter(Mandatory)]
        [string]$SourceDir
    )
    Write-SubStep "Building zlib..."
    $zlibBuild = 'zlibbuild'
    New-Item -ItemType Directory -Path $zlibBuild -Force | Out-Null
    Push-Location $zlibBuild
    try {
        $zlibFlags = @(
            "-DCMAKE_BUILD_TYPE=Release"
            "-DCMAKE_INSTALL_PREFIX=$(Join-Path $PWD 'install')"
            "-DZLIB_BUILD_TESTING=OFF"
            "-DZLIB_BUILD_SHARED=OFF"
            "-DZLIB_BUILD_STATIC=ON"
            "-DZLIB_INSTALL=ON"
            "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
        )
        $cache = Write-CMakeCacheFile -Flags $zlibFlags -FileName 'zlib_cache.cmake'
        $otherFlags = $cache.OtherFlags
        Invoke-NativeCommand cmake -GNinja -C $cache.CacheFile @otherFlags $SourceDir
        Invoke-NativeCommand $script:NinjaCommand @script:NinjaExtraArgs install
        $script:ZlibInstallDir = Get-ForwardSlashPath (Join-Path $PWD 'install')
    } finally {
        Pop-Location
    }
}

function Build-Zstd {
    <#
    .SYNOPSIS
        Builds zstd and sets $script:ZstdInstallDir to the install path.
    .NOTES
        See Build-LibXml2 for why a script-scoped variable is used instead of return.
    #>
    param(
        [Parameter(Mandatory)]
        [string]$SourceDir
    )
    Write-SubStep "Building zstd..."
    $zstdBuild = 'zstdbuild'
    New-Item -ItemType Directory -Path $zstdBuild -Force | Out-Null
    Push-Location $zstdBuild
    try {
        $zstdFlags = @(
            "-DCMAKE_BUILD_TYPE=Release"
            "-DCMAKE_INSTALL_PREFIX=$(Join-Path $PWD 'install')"
            "-DZSTD_BUILD_PROGRAMS=ON"
            "-DZSTD_BUILD_TESTS=OFF"
            "-DZSTD_BUILD_STATIC=ON"
            "-DZSTD_BUILD_SHARED=OFF"
            # zstd's own CMakeLists.txt declares cmake_minimum_required(VERSION 3.10),
            # below the 3.15 that introduced CMP0091 -- so it defaults to OLD, and
            # CMAKE_MSVC_RUNTIME_LIBRARY below is silently ignored, leaving zstd on
            # CMake's traditional default (/MD, dynamic CRT). That mismatches the
            # rest of this build, which LLVM_ENABLE_RPMALLOC forces to /MT (see
            # LLVM_ENABLE_RPMALLOC's handling in llvm/CMakeLists.txt) -- producing
            # LNK4217 warnings ("locally defined symbol imported") for malloc/
            # calloc/free/etc. when zstd_static.lib links into llvm.exe. Forcing the
            # policy default (independent of zstd's own cmake_minimum_required)
            # makes CMAKE_MSVC_RUNTIME_LIBRARY actually take effect.
            "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW"
            "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
        )
        $cache = Write-CMakeCacheFile -Flags $zstdFlags -FileName 'zstd_cache.cmake'
        $otherFlags = $cache.OtherFlags
        Invoke-NativeCommand cmake -GNinja -C $cache.CacheFile @otherFlags "$SourceDir/build/cmake"
        Invoke-NativeCommand $script:NinjaCommand @script:NinjaExtraArgs install
        $script:ZstdInstallDir = Get-ForwardSlashPath (Join-Path $PWD 'install')
    } finally {
        Pop-Location
    }
}

function New-PGOProfile {
    <#
    .SYNOPSIS
        Builds an instrumented clang, trains it, and sets $script:PGOProfilePath.
    .NOTES
        Uses a script-scoped variable instead of return to avoid PowerShell
        capturing native command stdout as part of the function's return value,
        which would break Ninja's \r progress display and corrupt the path.

        PGO artifacts are created as top-level siblings of the stage1 build
        directory so that each step owns an independent directory tree:
          $InstrumentDir/   - instrumented clang build
          $TrainDir/        - training build
          $ProfilePath      - merged profile data
    #>
    param(
        [string[]]$CMakeFlags,
        [string]$Stage0BinDir,
        [string]$LlvmSrc,
        [Parameter(Mandatory)][string]$InstrumentDir,
        [Parameter(Mandatory)][string]$TrainDir,
        [Parameter(Mandatory)][string]$ProfilePath
    )
    Write-Step "Generating PGO profile"

    # Build instrumented Clang
    Write-SubStep "Building instrumented Clang..."
    New-Item -ItemType Directory -Path $InstrumentDir -Force | Out-Null
    Push-Location $InstrumentDir
    try {
        # The instrumented build only needs to produce an instrumented clang;
        # strip runtimes/projects that are unnecessary and would themselves be
        # compiled with -fprofile-generate (requiring the profile runtime to link).
        $instrumentFlags = $CMakeFlags + @(
            "-DLLVM_TARGETS_TO_BUILD=Native"
            "-DLLVM_BUILD_INSTRUMENTED=IR"
            '-DLLVM_ENABLE_RUNTIMES=""'
            '-DLLVM_ENABLE_PROJECTS="clang;lld"'
        )
        $cache = Write-CMakeCacheFile -Flags $instrumentFlags -FileName 'instrument_cache.cmake'
        $otherFlags = $cache.OtherFlags
        Invoke-NativeCommand cmake -GNinja -C $cache.CacheFile @otherFlags "$LlvmSrc/llvm"
        Invoke-WithRetry $script:NinjaCommand -Arguments (@($script:NinjaExtraArgs) + @('clang'))
        $instrumentedClang = Get-ForwardSlashPath (Join-Path $PWD 'bin' 'clang-cl.exe')
    } finally {
        Pop-Location
    }

    # Train: build part of LLVM with instrumented compiler
    Write-SubStep "Training with instrumented Clang..."
    New-Item -ItemType Directory -Path $TrainDir -Force | Out-Null
    Push-Location $TrainDir
    try {
        $trainFlags = $CMakeFlags + @(
            "-DCMAKE_C_COMPILER=$instrumentedClang"
            "-DCMAKE_CXX_COMPILER=$instrumentedClang"
            "-DLLVM_ENABLE_PROJECTS=clang"
            '-DLLVM_ENABLE_RUNTIMES=""'
            "-DLLVM_TARGETS_TO_BUILD=Native"
        )
        $cache = Write-CMakeCacheFile -Flags $trainFlags -FileName 'train_cache.cmake'
        $otherFlags = $cache.OtherFlags
        Invoke-NativeCommand cmake -GNinja -C $cache.CacheFile @otherFlags "$LlvmSrc/llvm"

        # Drop profiles generated from running cmake; those are not representative.
        Remove-Item -Path "$InstrumentDir/profiles/*.profraw" -Force -ErrorAction SilentlyContinue
        Invoke-NativeCommand $script:NinjaCommand @script:NinjaExtraArgs 'tools/clang/lib/Sema/CMakeFiles/obj.clangSema.dir/Sema.cpp.obj'
    } finally {
        Pop-Location
    }

    # Merge profiles
    $resolvedProfilePath = Get-ForwardSlashPath $ProfilePath
    Invoke-NativeCommand "$Stage0BinDir/llvm-profdata" merge `
        -output="$resolvedProfilePath" "$InstrumentDir/profiles/*.profraw"

    Write-SubStep "PGO profile generated: $resolvedProfilePath"
    $script:PGOProfilePath = $resolvedProfilePath
}

function Invoke-Build {
    <#
    .SYNOPSIS
        Runs ninja build with retries.  Honors $script:NinjaCommand and
        $script:NinjaExtraArgs (set via LLVM_NINJA_OVERRIDE).
    #>
    param(
        [string[]]$Targets = @()
    )
    if ($Targets.Count -eq 0) {
        Invoke-WithRetry $script:NinjaCommand -Arguments $script:NinjaExtraArgs
    } else {
        foreach ($target in $Targets) {
            Invoke-WithRetry $script:NinjaCommand -Arguments (@($script:NinjaExtraArgs) + @($target))
        }
    }
}

function Invoke-Tests {
    <#
    .SYNOPSIS
        Runs a list of test targets with retries, skipping as appropriate.
        On failure, retries re-run only the previously-failed tests where
        possible (see Get-LitRerunCommand) instead of the whole suite.
        Honors $script:NinjaCommand and $script:NinjaExtraArgs (set via
        LLVM_NINJA_OVERRIDE).
    #>
    param(
        [string[]]$Targets,
        [string]$Arch = ''
    )
    foreach ($target in $Targets) {
        # Skip runtime checks on non-amd64
        if ($target -eq 'check-runtimes' -and $Arch -ne 'amd64') {
            Write-SubStep "Skipping $target on $Arch"
            continue
        }
        Invoke-TestTarget -Target $target
    }
}

#===============================================================================
# Main build stages
#===============================================================================

function Build-Stage {
    <#
    .SYNOPSIS
        Builds a single stage (stage0 or stage1) for a given architecture.
    #>
    param(
        [Parameter(Mandatory)]
        [string]$Name,
        [Parameter(Mandatory)]
        [string[]]$CMakeFlags,
        [Parameter(Mandatory)]
        [string]$LlvmSrc,
        [string[]]$ExtraCMakeFlags = @(),
        [string[]]$TestTargets = @(),
        [string]$Arch = ''
    )

    Write-Step "Building $Name"
    New-Item -ItemType Directory -Path $Name -Force | Out-Null
    Push-Location $Name
    try {
        $allFlags = $CMakeFlags + $ExtraCMakeFlags
        $cache = Write-CMakeCacheFile -Flags $allFlags
        $otherFlags = $cache.OtherFlags
        Invoke-NativeCommand cmake -GNinja -C $cache.CacheFile @otherFlags "$LlvmSrc/llvm"
        Invoke-Build

        if ($TestTargets.Count -gt 0) {
            Invoke-Tests -Targets $TestTargets -Arch $Arch
        }
    } finally {
        Pop-Location
    }
}

function Build-Architecture {
    <#
    .SYNOPSIS
        Orchestrates the full 2-stage build for a given architecture.
    #>
    param(
        [Parameter(Mandatory)]
        [string]$Arch,
        [Parameter(Mandatory)]
        [string]$VsDevCmd,
        [Parameter(Mandatory)]
        [string]$LlvmSrc,
        [Parameter(Mandatory)]
        [string]$BuildDir,
        [Parameter(Mandatory)]
        [string]$PackageVersion,
        [Parameter(Mandatory)]
        [string[]]$CommonCMakeFlags,
        [Parameter(Mandatory)]
        [string]$CommonCompilerFlags,
        [string[]]$CommonLLDBFlags = @(),
        [switch]$UseForceMSVC
    )

    Write-Step "Building for architecture: $Arch"

    # Restore clean PATH and setup environment
    $env:PATH = $script:OriginalPath
    $pythonHome = Find-Python -Arch $Arch
    Enter-VsDevEnvironment -VsDevCmd $VsDevCmd -Arch $Arch
    $env:VSCMD_START_DIR = $BuildDir

    # Directory names (always computed, used by multiple steps)
    $stage0Name = "build_${Arch}_stage0"
    $stage1Name = "build_${Arch}"
    $instrumentDir = Join-Path $BuildDir "instrument_${Arch}"
    $trainDir      = Join-Path $BuildDir "train_${Arch}"
    $profileFile   = Join-Path $BuildDir "profile_${Arch}.profdata"

    #-------------------------------------------------------------------
    # Step: libxml2
    #-------------------------------------------------------------------
    if (Test-ShouldRun 'libxml2') {
        if (Test-IsStartStep 'libxml2') {
            Remove-StepDirectory (Join-Path $BuildDir $stage0Name 'libxmlbuild')
            Remove-StepDirectory (Join-Path $BuildDir $stage0Name 'zlibbuild')
            Remove-StepDirectory (Join-Path $BuildDir $stage0Name 'zstdbuild')
        }
        New-Item -ItemType Directory -Path $stage0Name -Force | Out-Null
        Push-Location $stage0Name
        try {
            Build-LibXml2 -SourceDir "$BuildDir/libxml2-v2.9.12"
            Build-Zlib -SourceDir "$BuildDir/zlib-1.3.2"
            Build-Zstd -SourceDir "$BuildDir/zstd-1.5.7"
        } finally {
            Pop-Location
        }
    } else {
        Write-Step "Skipping libxml2 (-StartAt $($script:StartAtStep))"
        $libxmlInstallPath = Join-Path $BuildDir $stage0Name 'libxmlbuild' 'install'
        $zlibInstallPath = Join-Path $BuildDir $stage0Name 'zlibbuild' 'install'
        $zstdInstallPath = Join-Path $BuildDir $stage0Name 'zstdbuild' 'install'
        Assert-PathExists -Path $libxmlInstallPath -Description 'libxml2 install directory'
        Assert-PathExists -Path $zlibInstallPath -Description 'zlib install directory'
        Assert-PathExists -Path $zstdInstallPath -Description 'zstd install directory'
        $script:LibXmlInstallDir = Get-ForwardSlashPath $libxmlInstallPath
        $script:ZlibInstallDir = Get-ForwardSlashPath $zlibInstallPath
        $script:ZstdInstallDir = Get-ForwardSlashPath $zstdInstallPath
    }
    $libxmlDir = $script:LibXmlInstallDir
    $zlibDir = $script:ZlibInstallDir
    $zstdDir = $script:ZstdInstallDir

    # Compute stage0BinDir (always needed by later steps)
    $stage0BinDir = Get-ForwardSlashPath (Join-Path $BuildDir $stage0Name 'bin')

    # Compute cmakeFlags (always needed, cheap -- derived from parameters)
    $cmakeFlags = $CommonCMakeFlags + @(
        "-DPython3_ROOT_DIR=$env:PYTHONHOME"
        "-DLIBXML2_INCLUDE_DIR=$libxmlDir/include/libxml2"
        "-DLIBXML2_LIBRARY=$libxmlDir/lib/libxml2s.lib"
        "-DLIBXML2_LIBRARIES=$libxmlDir/lib/libxml2s.lib;ws2_32.lib"
        "-DZLIB_INCLUDE_DIR=$zlibDir/include"
        "-DZLIB_LIBRARY=$zlibDir/lib/zs.lib"
        "-DZLIB_LIBRARY_RELEASE=$zlibDir/lib/zs.lib"
        "-Dzstd_INCLUDE_DIR=$zstdDir/include"
        "-Dzstd_LIBRARY=$zstdDir/lib/zstd_static.lib"
    )

    if ($Arch -eq 'x86') {
        $cmakeFlags += "-DLLVM_ENABLE_RPMALLOC=OFF"
    } else {
        $cmakeFlags += "-DCLANG_DEFAULT_LINKER=lld"
    }
    if ($Arch -eq 'arm64') {
        $cmakeFlags += "-DCOMPILER_RT_BUILD_SANITIZERS=OFF"
    }

    #-------------------------------------------------------------------
    # Step: stage0
    #-------------------------------------------------------------------
    if (Test-ShouldRun 'stage0') {
        if (Test-IsStartStep 'stage0') {
            # Clean stage0 build artifacts but preserve the libxmlbuild/ subdirectory
            $stage0Dir = Join-Path $BuildDir $stage0Name
            if (Test-Path $stage0Dir) {
                Write-SubStep "Cleaning stage0 build artifacts (preserving libxmlbuild/)..."
                Get-ChildItem -Path $stage0Dir -Exclude 'libxmlbuild' | Remove-Item -Recurse -Force
            }
        }

        $stage0Tests = if ($Arch -eq 'x86') {
            @('check-lld')
        } else {
            @('check-llvm', 'check-clang', 'check-lld', 'check-runtimes',
              'check-clang-tools', 'check-clangd')
        }

        # Stage0-only override, ON instead of the shared OFF: compiles here are
        # farmed out to distributed compilation agents that can't see this build
        # tree. The PGO instrument stage (New-PGOProfile) compiles with stage0's
        # clang-cl under -fprofile-generate, which embeds a
        # /DEFAULTLIB:clang_rt.profile...lib directive whose exact name clang
        # picks by probing the filesystem for stage0's own compiler-rt layout.
        # Objects compiled remotely can't see that layout and always fall back to
        # the per-target name; objects compiled locally succeed the probe and
        # embed whatever layout is actually on disk. Under OFF those two
        # disagree, and the instrument build's link dies with "could not open
        # 'clang_rt.profile.lib'" the moment it hits a remotely-compiled object.
        # ON makes probe-success and probe-fallback resolve to the same name, so
        # local and remote objects always agree. This only reshapes stage0's own
        # (throwaway) install, not the final shipped compiler-rt layout that
        # UnrealBuildTool depends on -- see the OFF left in $commonCMakeFlags.
        $stage0Extra = @("-DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=ON")
        if ($Arch -ne 'x86') { $stage0Extra += "-DLLVM_TARGETS_TO_BUILD=Native" }

        Build-Stage -Name $stage0Name `
            -CMakeFlags $cmakeFlags `
            -LlvmSrc $LlvmSrc `
            -ExtraCMakeFlags $stage0Extra `
            -TestTargets $stage0Tests `
            -Arch $Arch
    } else {
        Write-Step "Skipping stage0 (-StartAt $($script:StartAtStep))"
        Assert-PathExists -Path "$stage0BinDir/clang-cl.exe" -Description 'stage0 clang-cl.exe'
    }

    # Stage 1 compiler flags (always computed, derived from stage0 paths)
    $stage1CompilerFlags = @(
        "-DCMAKE_C_COMPILER=$stage0BinDir/clang-cl.exe"
        "-DCMAKE_CXX_COMPILER=$stage0BinDir/clang-cl.exe"
        "-DCMAKE_LINKER=$stage0BinDir/lld-link.exe"
        "-DCMAKE_AR=$stage0BinDir/llvm-lib.exe"
        "-DCMAKE_RC_COMPILER=$stage0BinDir/llvm-rc.exe"
    )

    if ($Arch -eq 'arm64') {
        $stage1CompilerFlags += "-DCPACK_SYSTEM_NAME=woa64"
    }

    $stage1Flags = $cmakeFlags + $stage1CompilerFlags

    #-------------------------------------------------------------------
    # Step: pgo (only for 64-bit builds)
    #-------------------------------------------------------------------
    $profileFlags = @()
    if ($Arch -ne 'x86') {
        if (Test-ShouldRun 'pgo') {
            if (Test-IsStartStep 'pgo') {
                Remove-StepDirectory $instrumentDir
                Remove-StepDirectory $trainDir
                Remove-StepDirectory $profileFile
            }
            New-PGOProfile -CMakeFlags $stage1Flags `
                -Stage0BinDir $stage0BinDir -LlvmSrc $LlvmSrc `
                -InstrumentDir $instrumentDir `
                -TrainDir $trainDir `
                -ProfilePath $profileFile
        } else {
            Write-Step "Skipping PGO (-StartAt $($script:StartAtStep))"
            Assert-PathExists -Path $profileFile -Description 'PGO profile data'
            $script:PGOProfilePath = Get-ForwardSlashPath $profileFile
        }
        $profilePath = $script:PGOProfilePath

        $pgoCFlags = "$CommonCompilerFlags -Wno-backend-plugin"
        $profileFlags = @(
            "-DLLVM_PROFDATA_FILE=$profilePath"
            "-DCMAKE_C_FLAGS=`"$pgoCFlags`""
            "-DCMAKE_CXX_FLAGS=`"$pgoCFlags`""
        )
    }

    #-------------------------------------------------------------------
    # Step: stage1
    #-------------------------------------------------------------------
    $stage1Projects = if ($Arch -eq 'x86') {
        '"clang;clang-tools-extra;lld;lldb"'
    } else {
        '"clang;clang-tools-extra;lld;lldb;flang;mlir"'
    }

    $stage1Extra = @(
        "-DLLVM_ENABLE_PROJECTS=$stage1Projects"
        "-DPYTHON_HOME=$env:PYTHONHOME"
        "-DLLVM_TOOL_LLVM_DRIVER_BUILD=ON"
        "-DLLVM_INSTALL_DRIVER_ALIASES=OFF"
    ) + $CommonLLDBFlags + $profileFlags

    if (Test-ShouldRun 'stage1') {
        if (Test-IsStartStep 'stage1') {
            Remove-StepDirectory (Join-Path $BuildDir $stage1Name)
        }

        $stage1Tests = if ($Arch -eq 'x86') {
            @('check-lld')
        } else {
            @('check-llvm', 'check-clang', 'check-lld', 'check-runtimes',
              'check-clang-tools', 'check-clangd')
        }

        Build-Stage -Name $stage1Name `
            -CMakeFlags $stage1Flags `
            -LlvmSrc $LlvmSrc `
            -ExtraCMakeFlags $stage1Extra `
            -TestTargets $stage1Tests `
            -Arch $Arch
    } else {
        Write-Step "Skipping stage1 (-StartAt $($script:StartAtStep))"
        Assert-PathExists -Path (Join-Path $BuildDir $stage1Name) -Description 'stage1 build directory'
    }

    #-------------------------------------------------------------------
    # Step: package (NSIS installer)
    #-------------------------------------------------------------------
    if (Test-ShouldRun 'package') {
        Write-Step "Creating NSIS installer package"
        Push-Location $stage1Name
        try {
            # Reconfigure before packaging rather than relying on whatever is
            # already in this build directory's cache.  The tarball step below
            # shares this same directory and reconfigures it with
            # LLVM_INSTALL_TOOLCHAIN_ONLY=OFF and LLVM_INSTALL_DRIVER_ALIASES=ON;
            # those values persist, so a later run that reaches "package" again
            # (a re-run, or -StartAt package) would otherwise inherit them and
            # silently produce a much larger installer -- one that ships the
            # development headers and bakes in every tool alias as a full copy,
            # instead of materializing the aliases at install time.
            # Passing the stage1 flags explicitly pins both back.
            $packageFlags = $stage1Flags + $stage1Extra
            $cache = Write-CMakeCacheFile -Flags $packageFlags -FileName 'package_cache.cmake'
            $otherFlags = $cache.OtherFlags
            Invoke-NativeCommand cmake -GNinja -C $cache.CacheFile @otherFlags "$LlvmSrc/llvm"

            # Wipe CPack's staging tree as well.  Fixing the cache above is not
            # sufficient on its own: CPack overwrites the files it installs but
            # never deletes ones it no longer installs, so anything staged by a
            # previous run with different settings (the tool aliases, the
            # development headers) would otherwise still be picked up and
            # packaged.  Every other step already cleans its own directory on
            # restart; this is the equivalent for "package".
            Remove-StepDirectory (Join-Path $PWD '_CPack_Packages')

            Invoke-NativeCommand $script:NinjaCommand @script:NinjaExtraArgs package
        } finally {
            Pop-Location
        }
    } else {
        Write-Step "Skipping package (-StartAt $($script:StartAtStep))"
    }

    #-------------------------------------------------------------------
    # Step: tarball
    #-------------------------------------------------------------------
    if ($Arch -ne 'x86') {
        $tripleArch = if ($Arch -eq 'amd64') { 'x86_64' } else { 'aarch64' }
        $filename = "clang+llvm-${PackageVersion}-${tripleArch}-pc-windows-msvc"

        if (Test-ShouldRun 'tarball') {
            if (Test-IsStartStep 'tarball') {
                Remove-StepDirectory (Join-Path $BuildDir $filename)
            }

            Push-Location $stage1Name
            try {
                $tarballFlags = $stage1Flags + $profileFlags + @(
                    "-DLLVM_INSTALL_TOOLCHAIN_ONLY=OFF"
                    "-DCMAKE_INSTALL_PREFIX=$BuildDir/$filename"
                    "-DLLVM_INCLUDE_TESTS=OFF"
                    # A plain archive has no installer to materialize aliases,
                    # so bake them in.  This overrides the OFF value that the
                    # "package" step configures into this shared build
                    # directory -- and which that step re-pins on every run,
                    # precisely because this one leaves ON behind here.
                    "-DLLVM_INSTALL_DRIVER_ALIASES=ON"
                )
                $cache = Write-CMakeCacheFile -Flags $tarballFlags -FileName 'tarball_cache.cmake'
                $otherFlags = $cache.OtherFlags
                Invoke-NativeCommand cmake -GNinja -C $cache.CacheFile @otherFlags "$LlvmSrc/llvm"
                Invoke-NativeCommand $script:NinjaCommand @script:NinjaExtraArgs install

                # Verify llvm-config works
                Invoke-NativeCommand "$BuildDir/$filename/bin/llvm-config.exe" --bindir
            } finally {
                Pop-Location
            }

            # Create compressed tarball
            Invoke-NativeCommand 7z a -ttar -so "$filename.tar" $filename `
                | 7z a -txz -si "$filename.tar.xz"
        } else {
            Write-Step "Skipping tarball (-StartAt $($script:StartAtStep))"
        }
    }
}

#===============================================================================
# Main
#===============================================================================

if ($Help) {
    Get-Help $MyInvocation.MyCommand.Path -Detailed
    exit 0
}

# Install prerequisites if requested
if ($InstallPrerequisites) {
    Install-Prerequisites
}

# Validate all required tools are available (always runs, even without -InstallPrerequisites).
Assert-Prerequisites

# Detect Visual Studio
$vsDevCmd = Find-VisualStudio

# Determine LLVM source directory
if ($DownloadSource) {
    $llvmSrc = $null  # Will be set after checkout
} else {
    $llvmSrc = Resolve-Path (Join-Path $PSScriptRoot '..\..\..') | Select-Object -ExpandProperty Path
}

# Auto-detect or validate version
if ($Version) {
    $packageVersion = $Version
    Write-Host "Using specified version: $packageVersion"
} else {
    if ($llvmSrc) {
        $versionInfo = Get-LLVMVersionFromSource -SourceDir $llvmSrc
        $packageVersion = $versionInfo.Full
        Write-Host "Auto-detected LLVM version from source: $packageVersion"
    } else {
        # -DownloadSource without -Version: try detecting from the repo the script lives in.
        $repoRoot = Join-Path $PSScriptRoot '..\..\..'
        if (Test-Path (Join-Path $repoRoot 'cmake' 'Modules' 'LLVMVersion.cmake')) {
            $versionInfo = Get-LLVMVersionFromSource -SourceDir (Resolve-Path $repoRoot)
            $packageVersion = $versionInfo.Full
            Write-Host "Auto-detected LLVM version from repo: $packageVersion"
        } else {
            Write-Error "Cannot auto-detect version. Use -Version <version> when using -DownloadSource."
            exit 1
        }
    }
}

$revision = "llvmorg-$packageVersion"
$buildDir = Join-Path $PWD "llvm_package_$packageVersion"

Write-Step "Configuration"
Write-Host "  Revision:        $revision"
Write-Host "  Package version: $packageVersion"
Write-Host "  Build dir:       $buildDir"
Write-Host "  Architectures:   $((@('x86','x64','arm64') | Where-Object { (Get-Variable $_ -ValueOnly) }) -join ', ')"

if ($StartAt) {
    # Resuming from a specific step: validate that the build directory exists.
    if (-not (Test-Path $buildDir)) {
        Write-Error "Build directory does not exist: $buildDir`nCannot use -StartAt without a previous build. Run a full build first."
        exit 1
    }
    Write-Host "  Resuming from step: $StartAt" -ForegroundColor Yellow
} else {
    # Full build: prompt to delete existing build directory.
    if (Test-Path $buildDir) {
        $answer = Read-Host "Build directory already exists: $buildDir`nDelete and re-create it? [y/N]"
        if ($answer -ne 'y' -and $answer -ne 'Y') {
            Write-Host "Aborted."
            exit 1
        }
        Remove-Item -Recurse -Force $buildDir
    }
}

New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
Push-Location $buildDir

try {
    # Download source if requested (skip when resuming with -StartAt)
    if ($DownloadSource -and -not $StartAt) {
        Write-Step "Downloading $revision"
        Invoke-NativeCommand curl.exe -L `
            "https://github.com/llvm/llvm-project/archive/$revision.zip" -o src.zip
        Invoke-NativeCommand 7z x src.zip
        Get-ChildItem -Directory -Filter 'llvm-project-*' |
            Rename-Item -NewName 'llvm-project'
        $llvmSrc = Join-Path $buildDir 'llvm-project'
    }

    # Download and extract libxml2 (skip if resuming and it already exists)
    if ($StartAt -and (Test-Path 'libxml2-v2.9.12')) {
        Write-Step "Skipping libxml2 download (already exists)"
    } else {
        Write-Step "Downloading libxml2"
        Invoke-NativeCommand curl.exe --remote-name `
            'https://gitlab.gnome.org/GNOME/libxml2/-/archive/v2.9.12/libxml2-v2.9.12.tar.gz'
        Test-FileChecksum -Path 'libxml2-v2.9.12.tar.gz' -Algorithm SHA256 `
            -ExpectedHash '98BFA7A9A5E2A75638422050740448EE9F02BF4DC2075C9822D7747D5FF9E617'
        Invoke-NativeCommand tar zxf 'libxml2-v2.9.12.tar.gz'
    }

    # Download and extract zlib (skip if resuming and it already exists)
    if ($StartAt -and (Test-Path 'zlib-1.3.2')) {
        Write-Step "Skipping zlib download (already exists)"
    } else {
        Write-Step "Downloading zlib"
        Invoke-NativeCommand curl.exe -LO `
            'https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz'
        Test-FileChecksum -Path 'zlib-1.3.2.tar.gz' -Algorithm SHA256 `
            -ExpectedHash 'BB329A0A2CD0274D05519D61C667C062E06990D72E125EE2DFA8DE64F0119D16'
        Invoke-NativeCommand tar zxf 'zlib-1.3.2.tar.gz'
    }

    # Download and extract zstd (skip if resuming and it already exists)
    if ($StartAt -and (Test-Path 'zstd-1.5.7')) {
        Write-Step "Skipping zstd download (already exists)"
    } else {
        Write-Step "Downloading zstd"
        Invoke-NativeCommand curl.exe -LO `
            'https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz'
        Test-FileChecksum -Path 'zstd-1.5.7.tar.gz' -Algorithm SHA256 `
            -ExpectedHash 'EB33E51F49A15E023950CD7825CA74A4A2B43DB8354825AC24FC1B7EE09E6FA3'
        # 'tests' directory excluded because of symlinks.
        Invoke-NativeCommand tar zxf 'zstd-1.5.7.tar.gz' --exclude 'tests/*'
    }

    # Preserve original PATH
    $script:OriginalPath = $env:PATH

    # Common flags
    $commonCompilerFlags = '-DLIBXML_STATIC -D_SILENCE_NONFLOATING_COMPLEX_DEPRECATION_WARNING'
    # HandleLLVMOptions.cmake appends /W4 (clang-cl's warning-level flag,
    # which re-enables -Wunused-template) to CMAKE_CXX_FLAGS *after* whatever
    # we seed it with here, and Clang resolves conflicting -W flags by last-one-
    # wins -- so putting -Wno-unused-template in $commonCompilerFlags above has
    # no effect. CMAKE_CXX_FLAGS_RELEASE is the one flags variable CMake always
    # emits after the fully-accumulated CMAKE_CXX_FLAGS on the actual compile
    # line, so it's the only place a suppression like this reliably sticks.
    # Value below matches CMake's own MSVC/clang-cl Release default so we don't
    # lose /O2 /Ob2 /DNDEBUG by overriding the cache variable outright.
    $releaseCompilerFlags = '/O2 /Ob2 /DNDEBUG -Wno-unused-template'
    $commonCMakeFlags = @(
        "-DCMAKE_BUILD_TYPE=Release"
        "-DLLVM_ENABLE_ASSERTIONS=OFF"
        "-DLLVM_INSTALL_TOOLCHAIN_ONLY=ON"
        '-DLLVM_TARGETS_TO_BUILD="AArch64;ARM;X86;BPF;WebAssembly;RISCV;NVPTX"'
        "-DLLVM_BUILD_LLVM_C_DYLIB=ON"
        "-DPython3_FIND_REGISTRY=NEVER"
        "-DPACKAGE_VERSION=$packageVersion"
        '-DCMAKE_CL_SHOWINCLUDES_PREFIX="Note: including file: "'
        "-DLLVM_ENABLE_LIBXML2=FORCE_ON"
        "-DCLANG_ENABLE_LIBXML2=OFF"
        "-DLLVM_ENABLE_ZLIB=FORCE_ON"
        "-DLLVM_ENABLE_ZSTD=FORCE_ON"
        "-DCMAKE_C_FLAGS=`"$commonCompilerFlags`""
        "-DCMAKE_CXX_FLAGS=`"$commonCompilerFlags`""
        "-DCMAKE_CXX_FLAGS_RELEASE=`"$releaseCompilerFlags`""
        "-DLLVM_ENABLE_RPMALLOC=ON"
        '-DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra;lld"'
        '-DLLVM_ENABLE_RUNTIMES="compiler-rt;openmp"'
        "-DCOMPILER_RT_BUILD_ORC=OFF"
        # Stays OFF here: this is the layout UnrealBuildTool's Windows toolchain
        # (VCToolChain.cs) hardcodes for the ASan/UBSan runtime libs it links and
        # stages (lib/clang/<ver>/lib/windows/clang_rt.*-x86_64.*), so the shipped
        # release build must keep it. Stage0 forces it ON instead -- see the
        # comment on $stage0Extra below for why.
        "-DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF"
        "-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON"
        # Must be set at configure time, not just left to CPack's own default.
        # llvm/CMakeLists.txt gates its NSIS settings (icons, MODIFY_PATH,
        # solid-LZMA compression, uninstall-before-install) on
        # `CPACK_GENERATOR STREQUAL "NSIS"`, which is evaluated before
        # include(CPack) populates CPACK_GENERATOR -- so without this the whole
        # block is silently skipped and the installer loses all of it.
        "-DCPACK_GENERATOR=NSIS"
    )

    # Use clang-cl if available (unless forced to MSVC)
    if (-not $ForceMSVC) {
        $clangCl = Get-Command clang-cl -ErrorAction SilentlyContinue
        $lldLink = Get-Command lld-link -ErrorAction SilentlyContinue
        if ($clangCl -and $lldLink) {
            Write-SubStep "Using clang-cl + lld-link for stage0"
            $commonCompilerFlags += ' -fuse-ld=lld'
            $commonCMakeFlags += @(
                "-DCMAKE_C_COMPILER=clang-cl.exe"
                "-DCMAKE_CXX_COMPILER=clang-cl.exe"
                "-DCMAKE_LINKER=lld-link.exe"
                "-DLLVM_ENABLE_LLD=ON"
                "-DCMAKE_C_FLAGS=`"$commonCompilerFlags`""
                "-DCMAKE_CXX_FLAGS=`"$commonCompilerFlags`""
            )
        }
    }

    $commonLLDBFlags = @(
        "-DLLDB_RELOCATABLE_PYTHON=1"
        "-DLLDB_EMBED_PYTHON_HOME=OFF"
    )

    # Build each requested architecture
    $buildParams = @{
        VsDevCmd            = $vsDevCmd
        LlvmSrc             = $llvmSrc
        BuildDir            = $buildDir
        PackageVersion      = $packageVersion
        CommonCMakeFlags    = $commonCMakeFlags
        CommonCompilerFlags = $commonCompilerFlags
        CommonLLDBFlags     = $commonLLDBFlags
        UseForceMSVC        = $ForceMSVC
    }

    if ($x86)   { Build-Architecture -Arch 'x86'    @buildParams }
    if ($x64)   { Build-Architecture -Arch 'amd64'  @buildParams }
    if ($arm64) { Build-Architecture -Arch 'arm64'  @buildParams }

    Write-Step "Build complete!"
    Write-Host "Packages are in: $buildDir" -ForegroundColor Green

} finally {
    Pop-Location
    # Restore the console mode that was saved at script start.
    if ($null -ne $script:SavedConsoleMode) {
        try { [ConsoleMode]::Set($script:SavedConsoleMode) } catch {}
    }
}
