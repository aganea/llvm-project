<#
.SYNOPSIS
    Build and test the dynamic debugging demo in multiple configurations.

.DESCRIPTION
    Uses CMake to configure and build each configuration in its own directory
    under build\. CMake handles compiler detection; you just point it at the
    right compiler via -ClangCl for Clang configs.

    CMakeLists.txt defines DYNDBG_MODE=aot|hybrid with /dynamicdeopt:aot|:hybrid and
    CheckCXXCompilerFlag; build.ps1 skips clang-aot|clang-hybrid until LLVM implements
    those flags (see $ClangDynamicDebuggingImplemented).

    Configurations:
      msvc-baseline      cl.exe /O2 /Z7 (reference, no dynamic debugging)
      msvc-dynamicdeopt  cl.exe /O2 /Z7 /dynamicdeopt (MSVC ahead-of-time)
      clang-aot          CMake DYNDBG_MODE=aot — skipped until implemented
      clang-hybrid       CMake DYNDBG_MODE=hybrid — skipped until implemented

.PARAMETER Configs
    Which configurations to build. Default: all.

.PARAMETER ClangCl
    Path to clang-cl.exe for Clang configs. If not specified, Clang configs
    are skipped unless clang-cl is already on PATH.

.PARAMETER Clean
    Remove all build output before building.

.PARAMETER SkipTest
    Skip running ctest after building.

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Configs msvc-dynamicdeopt
    .\build.ps1 -Configs clang-aot -ClangCl C:\mybuild\bin\clang-cl.exe
    .\build.ps1 -Clean
#>
param(
    [string[]]$Configs = @("all"),
    [string]$ClangCl = "",
    [switch]$Clean,
    [switch]$SkipTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = $PSScriptRoot
$BuildRoot = "$ScriptDir\build"
# Default "all": MSVC-only. Use -Configs clang-aot,clang-hybrid to exercise skip / future LLVM.
$AllConfigs = @("msvc-baseline", "msvc-dynamicdeopt")

# Set $true after LLVM implements /dynamicdeopt:aot and /dynamicdeopt:hybrid (see demo CMakeLists.txt).
$ClangDynamicDebuggingImplemented = $false

if ($Clean -and (Test-Path $BuildRoot)) {
    Write-Host "Cleaning $BuildRoot..." -ForegroundColor Yellow
    Remove-Item $BuildRoot -Recurse -Force
}

$selected = if ($Configs -contains "all") { $AllConfigs } else { $Configs }

# ── Resolve clang-cl path ────────────────────────────────────────────────────

function Resolve-ClangCl {
    if ($ClangCl -and (Test-Path $ClangCl)) { return (Resolve-Path $ClangCl).Path }
    $found = Get-Command clang-cl -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    return $null
}

# CMake writes warnings to stderr; PowerShell wraps that in ErrorRecords. With
# $ErrorActionPreference = Stop, piping cmake 2>&1 can terminate the script.
# Run native cmake without merging stderr, or use Continue during the call.
function Invoke-CMakeConfigure {
    param([string[]]$CMakeArguments)
    # Capture output so it is not returned as part of the function's output stream
    # (otherwise the caller's $cfgExit becomes an array of strings + exit code).
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $null = & cmake @CMakeArguments 2>&1
        return [int]$LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
}

function Invoke-CMakeBuild {
    param([string]$BuildDir)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $null = & cmake --build $BuildDir 2>&1
        return [int]$LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
}

# Skip clang-aot / clang-hybrid until LLVM implements the flags (CMakeLists.txt uses CheckCXXCompilerFlag).
$clangConfigs = @("clang-aot", "clang-hybrid")
$clangResolved = Resolve-ClangCl
$selectedFiltered = [System.Collections.Generic.List[string]]::new()
foreach ($c in $selected) {
    if ($c -in $clangConfigs -and -not $ClangDynamicDebuggingImplemented) {
        Write-Warning "Skipping '${c}': LLVM /dynamicdeopt:aot and /dynamicdeopt:hybrid are not implemented yet. After Clang implements them, set `$ClangDynamicDebuggingImplemented = `$true` at the top of build.ps1 (CMakeLists.txt already uses CheckCXXCompilerFlag)."
        continue
    }
    if ($c -in $clangConfigs -and -not $clangResolved) {
        Write-Warning "Skipping '$c': clang-cl not found. Install LLVM/VS Clang, add clang-cl to PATH, or pass -ClangCl <path-to-clang-cl.exe>."
        continue
    }
    $selectedFiltered.Add($c) | Out-Null
}
$selected = @($selectedFiltered)
if ($selected.Count -eq 0) {
    Write-Warning "No configurations left to build (all were skipped). Exiting."
    exit 0
}

# ── Per-configuration build ──────────────────────────────────────────────────

function Invoke-Build([string]$Name, [string]$DyndbgMode, [string]$Compiler) {
    $outDir = "$BuildRoot\$Name"
    Write-Host ""
    Write-Host ("=" * 70) -ForegroundColor Green
    Write-Host "  $Name" -ForegroundColor Green
    Write-Host ("=" * 70) -ForegroundColor Green

    # CMake configure args
    $cmakeArgs = @(
        "-S", $ScriptDir
        "-B", $outDir
        "-G", "Ninja"
        "-DDYNDBG_MODE=$DyndbgMode"
        "-DCMAKE_BUILD_TYPE=Release"
    )

    if ($Compiler -eq "msvc") {
        # Let CMake find MSVC via the default VS generator/environment.
        # If Ninja is used, MSVC must already be in the environment (vcvars).
        $cmakeArgs += "-DCMAKE_C_COMPILER=cl"
        $cmakeArgs += "-DCMAKE_CXX_COMPILER=cl"
    }
    elseif ($Compiler -eq "clang") {
        $clang = Resolve-ClangCl
        if (-not $clang) {
            Write-Warning "clang-cl not found. Pass -ClangCl <path> or add clang-cl to PATH. Skipping $Name."
            return
        }
        Write-Host "  clang-cl: $clang" -ForegroundColor DarkGray
        $cmakeArgs += "-DCMAKE_C_COMPILER=$clang"
        $cmakeArgs += "-DCMAKE_CXX_COMPILER=$clang"
    }

    # Configure (do not pipe stderr: avoids PowerShell treating CMake warnings as fatal)
    Write-Host "  Configuring..." -ForegroundColor DarkGray
    $cfgExit = Invoke-CMakeConfigure -CMakeArguments $cmakeArgs
    if ($cfgExit -ne 0) { Write-Warning "CMake configure failed for $Name (exit $cfgExit)."; return }

    # Build
    Write-Host "  Building..." -ForegroundColor DarkGray
    $buildExit = Invoke-CMakeBuild -BuildDir $outDir
    if ($buildExit -ne 0) { Write-Warning "Build failed for $Name (exit $buildExit)."; return }

    # Report file sizes
    Write-Host ""
    Write-Host "  Artifacts:" -ForegroundColor Cyan
    $files = Get-ChildItem $outDir -File -Recurse | Where-Object {
        $_.Extension -in ".obj",".exe",".pdb",".bc",".bca",".ilk"
    } | Sort-Object Name
    [long]$totalKB = 0
    foreach ($f in $files) {
        $kb = [math]::Round($f.Length / 1KB, 1)
        $totalKB += $kb
        $tag = if ($f.Name -match '\.alt\.') { " (deoptimized)" } else { "" }
        Write-Host ("    {0,-40} {1,10:N1} KB{2}" -f $f.Name, $kb, $tag)
    }
    Write-Host ("    {0,-40} {1,10:N1} KB" -f "TOTAL", $totalKB) -ForegroundColor White

    # Test
    if (-not $SkipTest) {
        Write-Host ""
        Write-Host "  Running ctest..." -ForegroundColor Cyan
        $prev = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            & ctest --test-dir $outDir --output-on-failure
            if ($LASTEXITCODE -ne 0) { Write-Warning "Tests failed for $Name." }
        } finally {
            $ErrorActionPreference = $prev
        }
    }
}

# ── Main ─────────────────────────────────────────────────────────────────────

Write-Host ""
Write-Host "Dynamic Debugging Demo" -ForegroundColor White
Write-Host "Configs: $($selected -join ', ')" -ForegroundColor DarkGray

foreach ($cfg in $selected) {
    switch ($cfg) {
        "msvc-baseline"      { Invoke-Build $cfg "none"   "msvc"  }
        "msvc-dynamicdeopt"  { Invoke-Build $cfg "msvc"   "msvc"  }
        "clang-aot"          { Invoke-Build $cfg "aot"    "clang" }
        "clang-hybrid"       { Invoke-Build $cfg "hybrid" "clang" }
        default              { Write-Warning "Unknown config: $cfg" }
    }
}

# ── Summary ──────────────────────────────────────────────────────────────────

Write-Host ""
Write-Host ("=" * 70) -ForegroundColor White
Write-Host "  Summary" -ForegroundColor White
Write-Host ("=" * 70) -ForegroundColor White

foreach ($cfg in $selected) {
    $dir = "$BuildRoot\$cfg"
    if (-not (Test-Path $dir)) { continue }

    [long]$exeKB = 0; [long]$pdbKB = 0; [long]$altKB = 0; [long]$bcKB = 0
    Get-ChildItem $dir -File -Recurse | ForEach-Object {
        $kb = [math]::Round($_.Length / 1KB, 1)
        $n = $_.Name
        if     ($n -match '\.alt\.')       { $altKB += $kb }
        elseif ($n -match '\.dyndbg\.bc$') { $bcKB  += $kb }
        elseif ($n -like '*.exe')          { $exeKB += $kb }
        elseif ($n -like '*.pdb')          { $pdbKB += $kb }
    }
    $total = $exeKB + $pdbKB + $altKB + $bcKB
    Write-Host ""
    Write-Host "  $cfg" -ForegroundColor Cyan
    Write-Host ("    exe: {0,8:N0} KB   pdb: {1,8:N0} KB" -f $exeKB, $pdbKB)
    if ($altKB -gt 0) { Write-Host ("    .alt:  {0,8:N0} KB" -f $altKB) -ForegroundColor Yellow }
    if ($bcKB  -gt 0) { Write-Host ("    .bc:   {0,8:N0} KB" -f $bcKB)  -ForegroundColor Yellow }
    Write-Host ("    total: {0,8:N0} KB" -f $total) -ForegroundColor White
}

Write-Host ""
Write-Host "Done." -ForegroundColor Green
