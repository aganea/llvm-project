<#
.SYNOPSIS
    End-to-end LLDB test for the dynamic-debugging demo with all /dyndbg modes.

.DESCRIPTION
    Builds the multi-TU demo (math_utils.cpp, ipo_stress.cpp, main.cpp) with
    /dyndbg:{dynamic,hybrid,aot}, then for each mode:
      1. Checks build artifacts (exe, obj, pdb, alt.obj for aot)
      2. Runs the exe and verifies output correctness ("Final: 495")
      3. Tests dyndbg deoptimize/status/reoptimize on compute()
      4. Tests dyndbg deoptimize on IPO stress functions (dead_arg_helper,
         promote_helper, dispatch_helper, internal_callee) -- exercises
         preserve-abi guards
      5. Tests program correctness through deoptimized code paths
    Reports PASS/FAIL per assertion.

.PARAMETER BuildDir
    Path to the LLVM build directory. Default: c:\git\llvm-project\stage1

.PARAMETER SkipBuild
    Skip the compile+link step (reuse existing artifacts).

.EXAMPLE
    .\test_dyndbg.ps1
    .\test_dyndbg.ps1 -BuildDir C:\my\llvm-build
    .\test_dyndbg.ps1 -SkipBuild
#>
param(
    [string]$BuildDir = "c:\git\llvm-project\stage1",
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir  = $PSScriptRoot
$SrcDir     = Join-Path $ScriptDir "src"
$OutDir     = Join-Path $ScriptDir "test-out"
$ClangCl    = "$BuildDir\bin\clang-cl.exe"
$LldLink    = "$BuildDir\bin\lld-link.exe"
$Lldb       = "$BuildDir\bin\lldb.exe"

# ── Helpers ──────────────────────────────────────────────────────────────────

$script:PassCount = 0
$script:FailCount = 0
$script:TestResults = @()

function Assert-Contains {
    param(
        [string]$Haystack,
        [string]$Needle,
        [string]$TestName
    )
    if ($Haystack -match [regex]::Escape($Needle)) {
        $script:PassCount++
        $script:TestResults += [PSCustomObject]@{Test=$TestName; Result="PASS"}
        Write-Host "  PASS: $TestName" -ForegroundColor Green
    } else {
        $script:FailCount++
        $script:TestResults += [PSCustomObject]@{Test=$TestName; Result="FAIL"}
        Write-Host "  FAIL: $TestName" -ForegroundColor Red
        Write-Host "    Expected to find: '$Needle'" -ForegroundColor DarkRed
        $preview = if ($Haystack.Length -gt 2000) { "..." + $Haystack.Substring($Haystack.Length - 2000) } else { $Haystack }
        Write-Host "    Output was:`n$preview" -ForegroundColor DarkGray
    }
}

function Assert-NotContains {
    param(
        [string]$Haystack,
        [string]$Needle,
        [string]$TestName
    )
    if ($Haystack -match [regex]::Escape($Needle)) {
        $script:FailCount++
        $script:TestResults += [PSCustomObject]@{Test=$TestName; Result="FAIL"}
        Write-Host "  FAIL: $TestName" -ForegroundColor Red
        Write-Host "    Expected NOT to find: '$Needle'" -ForegroundColor DarkRed
    } else {
        $script:PassCount++
        $script:TestResults += [PSCustomObject]@{Test=$TestName; Result="PASS"}
        Write-Host "  PASS: $TestName" -ForegroundColor Green
    }
}

function Assert-Match {
    param(
        [string]$Haystack,
        [string]$Pattern,
        [string]$TestName
    )
    if ($Haystack -match $Pattern) {
        $script:PassCount++
        $script:TestResults += [PSCustomObject]@{Test=$TestName; Result="PASS"}
        Write-Host "  PASS: $TestName" -ForegroundColor Green
    } else {
        $script:FailCount++
        $script:TestResults += [PSCustomObject]@{Test=$TestName; Result="FAIL"}
        Write-Host "  FAIL: $TestName" -ForegroundColor Red
        Write-Host "    Expected regex: $Pattern" -ForegroundColor DarkRed
        $preview = if ($Haystack.Length -gt 2000) { "..." + $Haystack.Substring($Haystack.Length - 2000) } else { $Haystack }
        Write-Host "    Output was:`n$preview" -ForegroundColor DarkGray
    }
}

function Invoke-Lldb {
    param(
        [string]$Exe,
        [string[]]$Commands
    )
    $lldbArgs = @("-b", "--no-lldbinit")
    foreach ($cmd in $Commands) {
        $lldbArgs += "-o"
        $lldbArgs += $cmd
    }
    $lldbArgs += "--"
    $lldbArgs += $Exe

    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $Lldb @lldbArgs 2>&1 | Out-String
        return $output
    } finally {
        $ErrorActionPreference = $prev
    }
}

# ── Preflight ────────────────────────────────────────────────────────────────

foreach ($tool in @($ClangCl, $LldLink, $Lldb)) {
    if (-not (Test-Path $tool)) {
        Write-Error "Required tool not found: $tool"
        exit 1
    }
}

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  dynamic-debugging demo: end-to-end LLDB test" -ForegroundColor Cyan
Write-Host "  BuildDir: $BuildDir" -ForegroundColor DarkGray
Write-Host "  Sources:  $SrcDir" -ForegroundColor DarkGray
Write-Host "============================================================" -ForegroundColor Cyan

# ── Build ────────────────────────────────────────────────────────────────────

$Sources = @(
    (Join-Path $SrcDir "math_utils.cpp"),
    (Join-Path $SrcDir "ipo_stress.cpp"),
    (Join-Path $SrcDir "main.cpp")
)

$modes = @(
    @{Name="dynamic"; Flag="/dyndbg:dynamic"; ModeStr="dynamic"; AltObj=$false; Dyndbg=$false}
    @{Name="hybrid";  Flag="/dyndbg:hybrid";  ModeStr="hybrid";  AltObj=$false; Dyndbg=$true }
    @{Name="aot";     Flag="/dyndbg:aot";     ModeStr="aot";     AltObj=$true;  Dyndbg=$false}
)

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "Building all three modes..." -ForegroundColor Yellow
    foreach ($m in $modes) {
        $name = $m.Name
        $flag = $m.Flag

        Write-Host "  $name ($flag)..." -NoNewline

        $objFiles = @()
        $compileOk = $true
        $prev = $ErrorActionPreference; $ErrorActionPreference = "Continue"
        foreach ($src in $Sources) {
            $stem = [System.IO.Path]::GetFileNameWithoutExtension($src)
            $objFile = Join-Path $OutDir "$stem.$name.obj"
            $objFiles += $objFile
            $null = & $ClangCl /Z7 /O2 /EHsc $flag /I"$SrcDir" $src /c "/Fo$objFile" 2>&1
            if ($LASTEXITCODE -ne 0) { $compileOk = $false }
        }

        $exeFile = Join-Path $OutDir "demo.$name.exe"
        $null = & $LldLink /DEBUG:FULL @objFiles "/OUT:$exeFile" 2>&1
        $linkOk = $LASTEXITCODE -eq 0
        $ErrorActionPreference = $prev

        if ($compileOk -and $linkOk) {
            Write-Host " OK" -ForegroundColor Green
        } else {
            Write-Host " FAILED (compile=$compileOk link=$linkOk)" -ForegroundColor Red
            exit 1
        }
    }
}

# ── Artifact checks ─────────────────────────────────────────────────────────

Write-Host ""
Write-Host "--- Artifact checks ---" -ForegroundColor Yellow

foreach ($m in $modes) {
    $name = $m.Name

    $exeFile = Join-Path $OutDir "demo.$name.exe"
    $pdbFile = Join-Path $OutDir "demo.$name.pdb"

    Assert-Contains (Test-Path $exeFile).ToString() "True" "$name : exe exists"
    Assert-Contains (Test-Path $pdbFile).ToString() "True" "$name : pdb exists"

    foreach ($stem in @("math_utils", "ipo_stress", "main")) {
        $objFile = Join-Path $OutDir "$stem.$name.obj"
        Assert-Contains (Test-Path $objFile).ToString() "True" "$name : $stem.obj exists"
    }

    if ($m.AltObj) {
        foreach ($stem in @("math_utils", "ipo_stress", "main")) {
            $altFile = Join-Path $OutDir "$stem.aot.alt.obj"
            Assert-Contains (Test-Path $altFile).ToString() "True" "$name : $stem.alt.obj exists"
        }
    }
}

# Hybrid objs should be larger (embedded .dyndbg bitcode section)
$dynamicSize = (Get-Item (Join-Path $OutDir "math_utils.dynamic.obj")).Length
$hybridSize  = (Get-Item (Join-Path $OutDir "math_utils.hybrid.obj")).Length
if ($hybridSize -gt $dynamicSize) {
    $script:PassCount++
    $script:TestResults += [PSCustomObject]@{Test="hybrid obj > dynamic obj (embedded bitcode)"; Result="PASS"}
    Write-Host "  PASS: hybrid obj > dynamic obj (embedded bitcode)" -ForegroundColor Green
} else {
    $script:FailCount++
    $script:TestResults += [PSCustomObject]@{Test="hybrid obj > dynamic obj"; Result="FAIL"}
    Write-Host "  FAIL: hybrid obj ($hybridSize) should be larger than dynamic obj ($dynamicSize)" -ForegroundColor Red
}

# ── Runtime correctness ──────────────────────────────────────────────────────

Write-Host ""
Write-Host "--- Runtime correctness ---" -ForegroundColor Yellow

foreach ($m in $modes) {
    $name = $m.Name
    $exeFile = Join-Path $OutDir "demo.$name.exe"
    $prev = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    $runOutput = & $exeFile 2>&1 | Out-String
    $ec = $LASTEXITCODE
    $ErrorActionPreference = $prev

    Assert-Contains $runOutput "Final: 495"          "$name : output contains 'Final: 495'"
    Assert-Contains $runOutput "IPO stress tests:"   "$name : IPO stress tests ran"
    if ($ec -eq 0) {
        $script:PassCount++
        $script:TestResults += [PSCustomObject]@{Test="$name : exit code 0"; Result="PASS"}
        Write-Host "  PASS: $name : exit code 0" -ForegroundColor Green
    } else {
        $script:FailCount++
        $script:TestResults += [PSCustomObject]@{Test="$name : exit code 0"; Result="FAIL"}
        Write-Host "  FAIL: $name : exit code $ec (expected 0)" -ForegroundColor Red
    }
}

# ── LLDB tests per mode ─────────────────────────────────────────────────────

foreach ($m in $modes) {
    $name    = $m.Name
    $modeStr = $m.ModeStr
    $exeFile = Join-Path $OutDir "demo.$name.exe"

    Write-Host ""
    Write-Host "=== LLDB tests: /dyndbg:$name ===" -ForegroundColor Yellow

    # ── Test 1: deoptimize compute() + status + reoptimize ──────────────
    Write-Host "  -- deoptimize / status / reoptimize (compute) --" -ForegroundColor DarkCyan

    $output = Invoke-Lldb -Exe $exeFile -Commands @(
        "breakpoint set -n main"
        "run"
        "dyndbg deoptimize compute"
        "dyndbg status"
        "dyndbg reoptimize compute"
        "dyndbg status"
        "continue"
        "quit"
    )

    Assert-Match   $output "(?i)Mode:\s*$modeStr"         "$name/compute: mode detected as '$modeStr'"
    Assert-Contains $output "DONE [$modeStr]"              "$name/compute: DONE message with mode tag"
    Assert-Contains $output "deoptimized"                  "$name/compute: 'deoptimized' in output"
    Assert-Contains $output "[Deoptimized]"                "$name/compute/status: shows [Deoptimized]"
    Assert-Contains $output "compute"                      "$name/compute/status: shows compute"
    Assert-Contains $output "Restored optimized entry"     "$name/compute/reopt: restored message"
    Assert-Contains $output "No functions are currently deoptimized" "$name/compute/status-after: empty"

    # ── Test 2: deoptimize compute() + run to completion ─────────────────
    Write-Host "  -- deoptimize compute + run to completion --" -ForegroundColor DarkCyan

    $output2 = Invoke-Lldb -Exe $exeFile -Commands @(
        "breakpoint set -n main"
        "run"
        "dyndbg deoptimize compute"
        "continue"
        "quit"
    )

    Assert-Contains $output2 "DONE"                        "$name/run-compute: deoptimize succeeded"
    Assert-Contains $output2 "exited with status = 0"      "$name/run-compute: correct exit through deopt code"

    # ── Test 3: IPO stress -- deoptimize each static helper ──────────────
    #
    # These static functions are the targets of preserve-abi guards:
    #   dead_arg_helper   -> guarded by DeadArgElim
    #   promote_helper    -> guarded by ArgPromotion
    #   dispatch_helper   -> guarded by FunctionSpecialization
    #   internal_callee   -> guarded by GlobalOpt (CC change)
    #
    # For each one we deoptimize, then continue. If preserve-abi failed,
    # the function symbol won't exist in the PDB (ABI was changed or the
    # function was cloned/renamed), and the deoptimize will fail.

    $ipoFuncs = @("dead_arg_helper", "promote_helper", "dispatch_helper", "internal_callee")
    foreach ($fn in $ipoFuncs) {
        Write-Host "  -- IPO stress: deoptimize $fn --" -ForegroundColor DarkCyan

        $ipoOutput = Invoke-Lldb -Exe $exeFile -Commands @(
            "breakpoint set -n main"
            "run"
            "dyndbg deoptimize $fn"
            "dyndbg status"
            "dyndbg reoptimize $fn"
            "continue"
            "quit"
        )

        Assert-Contains $ipoOutput "DONE"                  "$name/ipo/${fn} - deoptimize succeeded"
        Assert-Contains $ipoOutput "[Deoptimized]"         "$name/ipo/${fn} - status shows [Deoptimized]"
        Assert-Contains $ipoOutput "Restored optimized"    "$name/ipo/${fn} - reoptimize succeeded"
        Assert-Contains $ipoOutput "exited with status = 0" "$name/ipo/${fn} - correct exit"
    }

    # ── Test 4: deoptimize ALL IPO stress funcs at once + run ────────────
    Write-Host "  -- IPO stress: all four deoptimized simultaneously --" -ForegroundColor DarkCyan

    $allIpoOutput = Invoke-Lldb -Exe $exeFile -Commands @(
        "breakpoint set -n main"
        "run"
        "dyndbg deoptimize dead_arg_helper"
        "dyndbg deoptimize promote_helper"
        "dyndbg deoptimize dispatch_helper"
        "dyndbg deoptimize internal_callee"
        "dyndbg status"
        "continue"
        "quit"
    )

    Assert-Contains $allIpoOutput "dead_arg_helper"        "$name/ipo-all: status shows dead_arg_helper"
    Assert-Contains $allIpoOutput "promote_helper"         "$name/ipo-all: status shows promote_helper"
    Assert-Contains $allIpoOutput "dispatch_helper"        "$name/ipo-all: status shows dispatch_helper"
    Assert-Contains $allIpoOutput "internal_callee"        "$name/ipo-all: status shows internal_callee"
    Assert-Contains $allIpoOutput "exited with status = 0" "$name/ipo-all: correct exit with all four deoptimized"
}

# ── Summary ──────────────────────────────────────────────────────────────────

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  Results: $($script:PassCount) passed, $($script:FailCount) failed" -ForegroundColor $(if ($script:FailCount -eq 0) { "Green" } else { "Red" })
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""

if ($script:FailCount -gt 0) {
    Write-Host "Failed tests:" -ForegroundColor Red
    $script:TestResults | Where-Object { $_.Result -eq "FAIL" } | ForEach-Object {
        Write-Host "  - $($_.Test)" -ForegroundColor Red
    }
    exit 1
}

exit 0
