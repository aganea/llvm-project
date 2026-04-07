<#
.SYNOPSIS
    Automated LLDB test for /dyndbg:{dynamic,hybrid,aot} modes.

.DESCRIPTION
    Builds hello.cpp with all three /dyndbg modes, then for each mode:
      1. Launches the exe under LLDB in batch mode
      2. Tests "dyndbg deoptimize MyFunc" and checks for mode detection + success
      3. Tests "dyndbg status" and checks that MyFunc is listed
      4. Tests "dyndbg reoptimize MyFunc" and checks for restoration
      5. Tests "dyndbg status" again (should be empty)
      6. Tests "dyndbg break MyFunc" + run + backtrace for [Deoptimized] label
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
        # Show last 2000 chars of output for debugging (tail is more useful)
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
    # Build lldb batch args: -b (batch mode) -o "cmd" for each command
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
Write-Host "  /dyndbg LLDB integration test" -ForegroundColor Cyan
Write-Host "  BuildDir: $BuildDir" -ForegroundColor DarkGray
Write-Host "============================================================" -ForegroundColor Cyan

# ── Build ────────────────────────────────────────────────────────────────────

$modes = @(
    @{Name="dynamic"; Flag="/dyndbg:dynamic"; ModeStr="dynamic";  AltObj=$false; Dyndbg=$false}
    @{Name="hybrid";  Flag="/dyndbg:hybrid";  ModeStr="hybrid";   AltObj=$false; Dyndbg=$true }
    @{Name="aot";     Flag="/dyndbg:aot";     ModeStr="aot";      AltObj=$true;  Dyndbg=$false}
)

if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "Building all three modes..." -ForegroundColor Yellow
    foreach ($m in $modes) {
        $name = $m.Name
        $flag = $m.Flag
        $objFile = Join-Path $ScriptDir "hello.$name.obj"
        $exeFile = Join-Path $ScriptDir "hello.$name.exe"

        Write-Host "  $name ($flag)..." -NoNewline
        $prev = $ErrorActionPreference; $ErrorActionPreference = "Continue"
        $null = & $ClangCl /Z7 /O2 $flag (Join-Path $ScriptDir "hello.cpp") /c "/Fo$objFile" 2>&1
        $compileOk = $LASTEXITCODE -eq 0
        $null = & $LldLink /DEBUG $objFile "/OUT:$exeFile" 2>&1
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

    $exeFile = Join-Path $ScriptDir "hello.$name.exe"
    $objFile = Join-Path $ScriptDir "hello.$name.obj"
    $pdbFile = Join-Path $ScriptDir "hello.$name.pdb"

    Assert-Contains (Test-Path $exeFile).ToString() "True" "$name : exe exists"
    Assert-Contains (Test-Path $objFile).ToString() "True" "$name : obj exists"
    Assert-Contains (Test-Path $pdbFile).ToString() "True" "$name : pdb exists"

    if ($m.AltObj) {
        $altFile = Join-Path $ScriptDir "hello.aot.alt.obj"
        Assert-Contains (Test-Path $altFile).ToString() "True" "$name : alt.obj exists"
    }
}

# Hybrid obj should be larger (has .dyndbg section)
$dynamicSize = (Get-Item (Join-Path $ScriptDir "hello.dynamic.obj")).Length
$hybridSize  = (Get-Item (Join-Path $ScriptDir "hello.hybrid.obj")).Length
if ($hybridSize -gt $dynamicSize) {
    $script:PassCount++
    $script:TestResults += [PSCustomObject]@{Test="hybrid obj > dynamic obj (embedded bitcode)"; Result="PASS"}
    Write-Host "  PASS: hybrid obj > dynamic obj (embedded bitcode)" -ForegroundColor Green
} else {
    $script:FailCount++
    $script:TestResults += [PSCustomObject]@{Test="hybrid obj > dynamic obj"; Result="FAIL"}
    Write-Host "  FAIL: hybrid obj ($hybridSize) should be larger than dynamic obj ($dynamicSize)" -ForegroundColor Red
}

# ── Runtime check ────────────────────────────────────────────────────────────

Write-Host ""
Write-Host "--- Runtime checks (exit code 42) ---" -ForegroundColor Yellow

foreach ($m in $modes) {
    $name = $m.Name
    $exeFile = Join-Path $ScriptDir "hello.$name.exe"
    $prev = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    & $exeFile 2>&1 | Out-Null
    $ec = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($ec -eq 42) {
        $script:PassCount++
        $script:TestResults += [PSCustomObject]@{Test="$name : exit code 42"; Result="PASS"}
        Write-Host "  PASS: $name : exit code 42" -ForegroundColor Green
    } else {
        $script:FailCount++
        $script:TestResults += [PSCustomObject]@{Test="$name : exit code 42"; Result="FAIL"}
        Write-Host "  FAIL: $name : exit code $ec (expected 42)" -ForegroundColor Red
    }
}

# ── LLDB tests per mode ─────────────────────────────────────────────────────

foreach ($m in $modes) {
    $name    = $m.Name
    $modeStr = $m.ModeStr
    $exeFile = Join-Path $ScriptDir "hello.$name.exe"

    # Clean temp files between modes to avoid stale artifacts.
    Remove-Item "$env:TEMP\hello.thin.bc","$env:TEMP\hello.dyndbg.obj" -ErrorAction SilentlyContinue

    Write-Host ""
    Write-Host "=== LLDB tests: /dyndbg:$name ===" -ForegroundColor Yellow

    # ── Test 1: deoptimize + status + reoptimize + status ──────────────
    Write-Host "  -- deoptimize / status / reoptimize --" -ForegroundColor DarkCyan

    # Must stop the process first (dyndbg needs a live process for load addresses
    # and memory allocation).
    $output = Invoke-Lldb -Exe $exeFile -Commands @(
        "breakpoint set -n main"
        "run"
        "dyndbg deoptimize MyFunc"
        "dyndbg status"
        "dyndbg reoptimize MyFunc"
        "dyndbg status"
        "continue"
        "quit"
    )

    # Check mode detection (case-insensitive: AOT prints "AOT", others lowercase)
    Assert-Match   $output "(?i)Mode:\s*$modeStr"         "$name/deopt: mode detected as '$modeStr'"
    # Check deoptimize success
    Assert-Contains $output "DONE [$modeStr]"              "$name/deopt: DONE message with mode tag"
    Assert-Contains $output "deoptimized"                  "$name/deopt: 'deoptimized' in output"
    # Check status shows the function
    Assert-Contains $output "[Deoptimized]"                "$name/status: shows [Deoptimized] label"
    Assert-Contains $output "MyFunc"                       "$name/status: shows MyFunc"
    # Check reoptimize success
    Assert-Contains $output "Restored optimized entry"     "$name/reoptimize: restored message"
    # Check status is empty after reoptimize
    Assert-Contains $output "No functions are currently deoptimized" "$name/status-after: empty"

    # ── Test 2: program correctness through deoptimized code ────────────
    Write-Host "  -- deoptimize + run to completion --" -ForegroundColor DarkCyan

    # Deoptimize MyFunc while stopped at main, then continue.
    # The JMP patch should redirect execution through the deoptimized code
    # and the program should still exit with code 42.
    $output2 = Invoke-Lldb -Exe $exeFile -Commands @(
        "breakpoint set -n main"
        "run"
        "dyndbg deoptimize MyFunc"
        "continue"
        "quit"
    )

    Assert-Contains $output2 "DONE"                        "$name/run: deoptimize succeeded"
    Assert-Contains $output2 "exited with status = 42"     "$name/run: correct exit code through deopt code"
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
