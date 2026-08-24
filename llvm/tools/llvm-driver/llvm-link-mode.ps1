<#
.SYNOPSIS
    Switch how LLVM tool aliases reference the executable they stand in for.

.DESCRIPTION
    After installing LLVM on Windows with the llvm-driver multicall binary,
    the bin/ directory contains llvm.exe plus aliases for each tool (clang.exe,
    lld.exe, llvm-ar.exe, etc.). A handful of other aliases duplicate a
    standalone tool that isn't part of llvm-driver at all (e.g. flang-new.exe
    is an alias of flang.exe). This script switches every alias between the
    following modes:

      hardlink  - NTFS hard links to the target executable.  Zero extra disk
                  usage.  Requires NTFS and same volume.  Won't survive VCS sync.
      symlink   - Symbolic links to the target executable.  Zero extra disk
                  usage.  Requires Developer Mode or administrator on Windows 10+.
      stub      - ~3 KB stub executables that spawn llvm.exe at runtime.
                  VCS-friendly, minimal disk, adds ~1 ms process-spawn latency.
                  Only supports aliases of llvm.exe -- the stub is hardcoded
                  to spawn it, so aliases of other tools (flang, offload-arch)
                  are skipped in this mode.
      copy      - Full copies of the target executable.  Universally
                  compatible but large.
      auto      - Unattended mode: try hardlink, then stub (aliases of
                  llvm.exe only), then copy, per tool, so no tool is ever
                  left missing.  Used by the NSIS installer's post-install step.
      remove    - Delete every alias listed in the manifest, without
                  replacement.  Used by the NSIS installer's uninstaller.

    The script reads llvm-driver-tools.txt (a build-time manifest listing
    "<alias> <target>" pairs, one per line) to know which executables to
    update and what each one is an alias of.

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
         (only the ~3 KB stubs are stored, not copies of llvm.exe; aliases of
         other tools such as flang-new.exe have no stub equivalent and are
         left untouched by this mode -- run `.\llvm-link-mode.ps1 copy` first
         if those need to be VCS-safe too, since a hard link left in place is
         stored by most VCS tools as a full copy of the target anyway)
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
    Write-Host '  hardlink  Hard links to the target executable (zero extra disk, NTFS only)'
    Write-Host '  symlink   Symbolic links to the target executable (needs Developer Mode or admin)'
    Write-Host '  stub      ~3 KB stub executables that spawn llvm.exe (VCS-friendly, llvm.exe aliases only)'
    Write-Host '  copy      Full copies of the target executable (universally compatible)'
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
    # Each non-empty line is "<alias> <target>", where <target> is the
    # executable (without .exe) that <alias> stands in for -- "llvm" for
    # every llvm-driver alias, or another standalone tool (e.g. flang,
    # offload-arch) for the handful of aliases that duplicate a tool that
    # wasn't folded into llvm-driver.
    $entries = @()
    foreach ($line in (Get-Content -LiteralPath $Manifest)) {
        $line = $line.Trim()
        if ($line -eq '') { continue }
        $parts = $line -split '\s+'
        if ($parts.Count -ne 2) {
            Write-Host "Warning: ignoring malformed manifest line: '$line'" -ForegroundColor Yellow
            continue
        }
        $entries += [PSCustomObject]@{ Alias = $parts[0]; Target = $parts[1] }
    }
    if ($entries.Count -eq 0) {
        Write-Host 'Error: manifest is empty.' -ForegroundColor Red
        exit 1
    }
    return $entries
}

# ── Status ───────────────────────────────────────────────────────────────────

function Show-Status {
    if (-not (Test-Path $LlvmExe)) {
        Write-Host "Error: llvm.exe not found in $BinDir" -ForegroundColor Red
        exit 1
    }
    $entries = Read-Manifest
    $targets = $entries | Select-Object -ExpandProperty Target -Unique

    foreach ($target in $targets) {
        $sample     = ($entries | Where-Object { $_.Target -eq $target })[0]
        $targetExe  = "$target.exe"
        $targetPath = Join-Path $BinDir $targetExe
        $samplePath = Join-Path $BinDir "$($sample.Alias).exe"

        Write-Host "Aliases of ${targetExe}:" -ForegroundColor Cyan

        if (-not (Test-Path $targetPath)) {
            Write-Host "  Status: MISSING - $targetExe does not exist" -ForegroundColor Yellow
            continue
        }
        if (-not (Test-Path $samplePath)) {
            Write-Host "  Status: MISSING - $($sample.Alias).exe does not exist" -ForegroundColor Yellow
            continue
        }

        # Symlink?
        if (Test-IsSymlink $samplePath) {
            Write-Host "  Status: SYMLINK ($($sample.Alias).exe -> $targetExe)" -ForegroundColor Green
            continue
        }

        # Hard link? (same file ID)
        $sampleId = Get-FileId $samplePath
        $targetId = Get-FileId $targetPath
        if ($null -ne $sampleId -and $null -ne $targetId -and $sampleId -eq $targetId) {
            Write-Host "  Status: HARDLINK ($($sample.Alias).exe and $targetExe share the same file ID)" -ForegroundColor Green
            continue
        }

        # Compare sizes to distinguish stub from copy.
        $targetSize = (Get-Item -LiteralPath $targetPath).Length
        $sampleSize = (Get-Item -LiteralPath $samplePath).Length

        if ($sampleSize -eq $targetSize) {
            Write-Host "  Status: COPY ($($sample.Alias).exe is a full copy of $targetExe, $targetSize bytes)" -ForegroundColor Yellow
            continue
        }

        if ($target -eq 'llvm' -and (Test-Path $StubExe) -and $sampleSize -eq (Get-Item -LiteralPath $StubExe).Length) {
            Write-Host "  Status: STUB ($($sample.Alias).exe is a stub, $sampleSize bytes)" -ForegroundColor Cyan
            continue
        }

        Write-Host '  Status: UNKNOWN' -ForegroundColor Yellow
        Write-Host "    $($sample.Alias).exe size: $sampleSize bytes"
        Write-Host "    $targetExe size:    $targetSize bytes"
    }
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

    $entries = Read-Manifest
    $success = 0
    $failed  = 0
    $skipped = 0

    Write-Host "Switching $($entries.Count) tool aliases to $TargetMode mode..." -ForegroundColor Cyan
    Write-Host ''

    foreach ($entry in $entries) {
        $alias      = $entry.Alias
        $target     = $entry.Target
        $toolExe    = "$alias.exe"
        $toolPath   = Join-Path $BinDir $toolExe
        $targetExe  = "$target.exe"
        $targetPath = Join-Path $BinDir $targetExe

        # The stub is hardcoded to spawn llvm.exe, so it can only stand in for
        # aliases of llvm.exe -- not aliases of other standalone tools such as
        # flang.exe or offload-arch.exe.
        if ($TargetMode -eq 'stub' -and $target -ne 'llvm') {
            Write-Host "  SKIPPED: $toolExe (stub mode only supports aliases of llvm.exe; this is an alias of $targetExe)" -ForegroundColor DarkGray
            $skipped++
            continue
        }

        if (-not (Test-Path $targetPath)) {
            Write-Host "  FAILED: $toolExe ($targetExe not found)" -ForegroundColor Red
            $failed++
            continue
        }

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
                & cmd /c "mklink /H `"$toolPath`" `"$targetPath`"" >$null 2>&1
                $ok = ($LASTEXITCODE -eq 0) -and (Test-Path $toolPath)
                if (-not $ok) {
                    Write-Host "  FAILED: $toolExe (hard link - wrong filesystem?)" -ForegroundColor Red
                }
            }
            'symlink' {
                # Relative symlink so the directory can be moved.
                & cmd /c "mklink `"$toolPath`" $targetExe" >$null 2>&1
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
                Copy-Item -LiteralPath $targetPath -Destination $toolPath -Force -ErrorAction SilentlyContinue
                $ok = Test-Path $toolPath
                if (-not $ok) {
                    Write-Host "  FAILED: $toolExe (copy failed)" -ForegroundColor Red
                }
            }
            'auto' {
                # Unattended fallback chain: hard link (zero extra disk), then
                # the stub if this is an alias of llvm.exe (near-zero disk),
                # then a full copy as the last resort -- a tool must never be
                # left missing after install.
                & cmd /c "mklink /H `"$toolPath`" `"$targetPath`"" >$null 2>&1
                $ok = ($LASTEXITCODE -eq 0) -and (Test-Path $toolPath)
                if (-not $ok -and $target -eq 'llvm' -and (Test-Path $StubExe)) {
                    Copy-Item -LiteralPath $StubExe -Destination $toolPath -Force -ErrorAction SilentlyContinue
                    $ok = Test-Path $toolPath
                }
                if (-not $ok) {
                    Copy-Item -LiteralPath $targetPath -Destination $toolPath -Force -ErrorAction SilentlyContinue
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
    Write-Host "Done: $success succeeded, $failed failed, $skipped skipped." -ForegroundColor $(if ($failed -gt 0) { 'Yellow' } else { 'Green' })

    if ($failed -gt 0) {
        Write-Host ''
        Write-Host 'Some aliases could not be updated.  Common causes:' -ForegroundColor Yellow
        Write-Host '  - The executable is currently running (close it first)'
        Write-Host '  - Insufficient permissions (try running as administrator)'
        Write-Host '  - Hard links require NTFS and the same volume as the target executable'
        Write-Host '  - Symlinks require Developer Mode or administrator privileges'
        exit 1
    }
}

# ── Remove ───────────────────────────────────────────────────────────────────

function Remove-Aliases {
    $entries = Read-Manifest
    $removed = 0
    $missing = 0
    $failed  = 0

    Write-Host "Removing $($entries.Count) tool aliases..." -ForegroundColor Cyan
    Write-Host ''

    foreach ($entry in $entries) {
        $toolExe  = "$($entry.Alias).exe"
        $toolPath = Join-Path $BinDir $toolExe
        if (-not (Test-Path $toolPath)) {
            $missing++
            continue
        }
        Remove-Item -LiteralPath $toolPath -Force -ErrorAction SilentlyContinue
        if (Test-Path $toolPath) {
            Write-Host "  FAILED: Cannot delete $toolExe (in use?)" -ForegroundColor Red
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
