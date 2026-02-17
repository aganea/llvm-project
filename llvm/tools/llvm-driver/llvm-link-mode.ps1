<#
.SYNOPSIS
    Switch how LLVM tool aliases reference llvm.exe.

.DESCRIPTION
    After installing LLVM on Windows with the llvm-driver multicall binary,
    the bin/ directory contains llvm.exe plus aliases for each tool (clang.exe,
    lld.exe, llvm-ar.exe, etc.).  This script switches those aliases between
    the following modes:

      hardlink  - NTFS hard links to llvm.exe.  Zero extra disk usage.
                  Requires NTFS and same volume.  Won't survive VCS sync.
      symlink   - Symbolic links to llvm.exe.  Zero extra disk usage.
                  Requires Developer Mode or administrator on Windows 10+.
      stub      - ~3 KB stub executables that spawn llvm.exe at runtime.
                  VCS-friendly, minimal disk, adds ~1 ms process-spawn latency.
      copy      - Full copies of llvm.exe.  Universally compatible but large.
      auto      - Unattended mode: try hardlink, then stub, then copy, per
                  tool, so no tool is ever left missing.  Used by the NSIS
                  installer's post-install step.
      remove    - Delete every alias listed in the manifest, without
                  replacement.  Used by the NSIS installer's uninstaller.

    The script reads llvm-driver-tools.txt (a build-time manifest listing
    every tool alias) to know which executables to update.

.PARAMETER Mode
    One of: hardlink, symlink, stub, copy, auto, remove, status.
    'status' reports the current mode without making changes.

.EXAMPLE
    .\llvm-link-mode.ps1 hardlink
    .\llvm-link-mode.ps1 stub
    .\llvm-link-mode.ps1 auto
    .\llvm-link-mode.ps1 remove
    .\llvm-link-mode.ps1 status

.NOTES
    Compatible with PowerShell 5.1 and later.

    VCS workflow:
      1. Run the NSIS installer (creates hard links by default).
      2. Before checking in: .\llvm-link-mode.ps1 stub
         (only the ~3 KB stubs are stored, not copies of llvm.exe)
      3. Post-sync hook:     .\llvm-link-mode.ps1 hardlink
         (recreates hard links for zero-overhead tool dispatch)
#>

param(
    [Parameter(Position = 0)]
    [ValidateSet('hardlink', 'symlink', 'stub', 'copy', 'auto', 'remove', 'status')]
    [string]$Mode
)

# ── Paths ────────────────────────────────────────────────────────────────────
$BinDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$Manifest = Join-Path $BinDir 'llvm-driver-tools.txt'
$LlvmExe  = Join-Path $BinDir 'llvm.exe'
$StubExe  = Join-Path $BinDir 'llvm-driver-stub.exe'

# ── Helpers ──────────────────────────────────────────────────────────────────

function Show-Usage {
    Write-Host ''
    Write-Host "Usage: $($MyInvocation.ScriptName) <hardlink|symlink|stub|copy|auto|remove|status>" -ForegroundColor Cyan
    Write-Host ''
    Write-Host '  hardlink  Hard links to llvm.exe (zero extra disk, NTFS only)'
    Write-Host '  symlink   Symbolic links to llvm.exe (needs Developer Mode or admin)'
    Write-Host '  stub      ~3 KB stub executables that spawn llvm.exe (VCS-friendly)'
    Write-Host '  copy      Full copies of llvm.exe (universally compatible)'
    Write-Host '  auto      Unattended: try hardlink, then stub, then copy per tool'
    Write-Host '  remove    Delete every alias, without replacement'
    Write-Host '  status    Show current mode without making changes'
    Write-Host ''
}

function Get-FileId {
    param([string]$Path)
    # Use fsutil to get the file ID; two hard links share the same ID.
    try {
        $output = & fsutil file queryfileid $Path 2>$null
        if ($LASTEXITCODE -eq 0 -and $output) {
            return $output.Trim()
        }
    } catch { }
    return $null
}

function Test-IsSymlink {
    param([string]$Path)
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if ($null -eq $item) { return $false }
    return ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0
}

function Read-Manifest {
    if (-not (Test-Path $Manifest)) {
        Write-Host "Error: llvm-driver-tools.txt not found in $BinDir" -ForegroundColor Red
        Write-Host 'This file is required to know which executables are tool aliases.'
        exit 1
    }
    # Read non-empty, non-whitespace lines.
    $tools = @(Get-Content -LiteralPath $Manifest |
               ForEach-Object { $_.Trim() } |
               Where-Object  { $_ -ne '' })
    if ($tools.Count -eq 0) {
        Write-Host 'Error: manifest is empty.' -ForegroundColor Red
        exit 1
    }
    return $tools
}

# ── Status ───────────────────────────────────────────────────────────────────

function Show-Status {
    if (-not (Test-Path $LlvmExe)) {
        Write-Host "Error: llvm.exe not found in $BinDir" -ForegroundColor Red
        exit 1
    }
    $tools = Read-Manifest
    $sample = $tools[0]
    $samplePath = Join-Path $BinDir "$sample.exe"

    if (-not (Test-Path $samplePath)) {
        Write-Host "Status: MISSING - $sample.exe does not exist" -ForegroundColor Yellow
        exit 1
    }

    # Symlink?
    if (Test-IsSymlink $samplePath) {
        Write-Host 'Status: SYMLINK' -ForegroundColor Green
        Write-Host "  $sample.exe is a symbolic link to llvm.exe"
        return
    }

    # Hard link? (same file ID)
    $sampleId = Get-FileId $samplePath
    $llvmId   = Get-FileId $LlvmExe
    if ($null -ne $sampleId -and $null -ne $llvmId -and $sampleId -eq $llvmId) {
        Write-Host 'Status: HARDLINK' -ForegroundColor Green
        Write-Host "  $sample.exe and llvm.exe share the same file ID"
        return
    }

    # Compare sizes to distinguish stub from copy.
    $llvmSize   = (Get-Item -LiteralPath $LlvmExe).Length
    $sampleSize = (Get-Item -LiteralPath $samplePath).Length

    if ($sampleSize -eq $llvmSize) {
        Write-Host 'Status: COPY' -ForegroundColor Yellow
        Write-Host "  $sample.exe is a full copy of llvm.exe ($llvmSize bytes)"
        return
    }

    if ((Test-Path $StubExe) -and $sampleSize -eq (Get-Item -LiteralPath $StubExe).Length) {
        Write-Host 'Status: STUB' -ForegroundColor Cyan
        Write-Host "  $sample.exe is a stub ($sampleSize bytes)"
        return
    }

    Write-Host 'Status: UNKNOWN' -ForegroundColor Yellow
    Write-Host "  $sample.exe size: $sampleSize bytes"
    Write-Host "  llvm.exe size:    $llvmSize bytes"
}

# ── Switch mode ──────────────────────────────────────────────────────────────

function Switch-Mode {
    param([string]$TargetMode)

    if (-not (Test-Path $LlvmExe)) {
        Write-Host "Error: llvm.exe not found in $BinDir" -ForegroundColor Red
        exit 1
    }

    if ($TargetMode -eq 'stub' -and -not (Test-Path $StubExe)) {
        Write-Host "Error: llvm-driver-stub.exe not found in $BinDir" -ForegroundColor Red
        Write-Host 'The stub executable is required for stub mode.'
        exit 1
    }

    $tools   = Read-Manifest
    $success = 0
    $failed  = 0

    Write-Host "Switching $($tools.Count) tool aliases to $TargetMode mode..." -ForegroundColor Cyan
    Write-Host ''

    foreach ($tool in $tools) {
        $toolExe  = "$tool.exe"
        $toolPath = Join-Path $BinDir $toolExe

        # Remove existing alias (whatever form it's in).
        if (Test-Path $toolPath) {
            Remove-Item -LiteralPath $toolPath -Force -ErrorAction SilentlyContinue
        }
        if (Test-Path $toolPath) {
            Write-Host "  FAILED: Cannot delete $toolExe (in use?)" -ForegroundColor Red
            $failed++
            continue
        }

        $ok = $false
        switch ($TargetMode) {
            'hardlink' {
                # cmd /c mklink is the simplest PS5-compatible way.
                & cmd /c "mklink /H `"$toolPath`" `"$LlvmExe`"" >$null 2>&1
                $ok = ($LASTEXITCODE -eq 0) -and (Test-Path $toolPath)
                if (-not $ok) {
                    Write-Host "  FAILED: $toolExe (hard link - wrong filesystem?)" -ForegroundColor Red
                }
            }
            'symlink' {
                # Relative symlink so the directory can be moved.
                & cmd /c "mklink `"$toolPath`" llvm.exe" >$null 2>&1
                $ok = ($LASTEXITCODE -eq 0) -and (Test-Path $toolPath)
                if (-not $ok) {
                    Write-Host "  FAILED: $toolExe (symlink - need Developer Mode or admin?)" -ForegroundColor Red
                }
            }
            'stub' {
                Copy-Item -LiteralPath $StubExe -Destination $toolPath -Force -ErrorAction SilentlyContinue
                $ok = Test-Path $toolPath
                if (-not $ok) {
                    Write-Host "  FAILED: $toolExe (copy failed)" -ForegroundColor Red
                }
            }
            'copy' {
                Copy-Item -LiteralPath $LlvmExe -Destination $toolPath -Force -ErrorAction SilentlyContinue
                $ok = Test-Path $toolPath
                if (-not $ok) {
                    Write-Host "  FAILED: $toolExe (copy failed)" -ForegroundColor Red
                }
            }
            'auto' {
                # Unattended fallback chain: hard link (zero extra disk), then
                # the stub (near-zero disk), then a full copy as the last
                # resort -- a tool must never be left missing after install.
                & cmd /c "mklink /H `"$toolPath`" `"$LlvmExe`"" >$null 2>&1
                $ok = ($LASTEXITCODE -eq 0) -and (Test-Path $toolPath)
                if (-not $ok -and (Test-Path $StubExe)) {
                    Copy-Item -LiteralPath $StubExe -Destination $toolPath -Force -ErrorAction SilentlyContinue
                    $ok = Test-Path $toolPath
                }
                if (-not $ok) {
                    Copy-Item -LiteralPath $LlvmExe -Destination $toolPath -Force -ErrorAction SilentlyContinue
                    $ok = Test-Path $toolPath
                }
                if (-not $ok) {
                    Write-Host "  FAILED: $toolExe (hard link, stub, and copy all failed)" -ForegroundColor Red
                }
            }
        }

        if ($ok) { $success++ } else { $failed++ }
    }

    Write-Host ''
    Write-Host "Done: $success succeeded, $failed failed." -ForegroundColor $(if ($failed -gt 0) { 'Yellow' } else { 'Green' })

    if ($failed -gt 0) {
        Write-Host ''
        Write-Host 'Some aliases could not be updated.  Common causes:' -ForegroundColor Yellow
        Write-Host '  - The executable is currently running (close it first)'
        Write-Host '  - Insufficient permissions (try running as administrator)'
        Write-Host '  - Hard links require NTFS and same volume as llvm.exe'
        Write-Host '  - Symlinks require Developer Mode or administrator privileges'
        exit 1
    }
}

# ── Remove ───────────────────────────────────────────────────────────────────

function Remove-Aliases {
    $tools   = Read-Manifest
    $removed = 0
    $missing = 0
    $failed  = 0

    Write-Host "Removing $($tools.Count) tool aliases..." -ForegroundColor Cyan
    Write-Host ''

    foreach ($tool in $tools) {
        $toolPath = Join-Path $BinDir "$tool.exe"
        if (-not (Test-Path $toolPath)) {
            $missing++
            continue
        }
        Remove-Item -LiteralPath $toolPath -Force -ErrorAction SilentlyContinue
        if (Test-Path $toolPath) {
            Write-Host "  FAILED: Cannot delete $tool.exe (in use?)" -ForegroundColor Red
            $failed++
        } else {
            $removed++
        }
    }

    Write-Host ''
    Write-Host "Done: $removed removed, $missing already absent, $failed failed." -ForegroundColor $(if ($failed -gt 0) { 'Yellow' } else { 'Green' })

    if ($failed -gt 0) {
        exit 1
    }
}

# ── Main ─────────────────────────────────────────────────────────────────────

if (-not $Mode) {
    Show-Usage
    exit 1
}

if ($Mode -eq 'status') {
    Show-Status
} elseif ($Mode -eq 'remove') {
    Remove-Aliases
} else {
    Switch-Mode -TargetMode $Mode
}
