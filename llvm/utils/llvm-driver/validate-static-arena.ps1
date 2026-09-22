#===-- validate-static-arena.ps1 - Inspect a folded driver -------*- ps1 -*-===#
#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
#===----------------------------------------------------------------------===#

[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string] $Executable,
  [string] $Pdb,
  [string] $PdbUtil,
  [string] $SelectorList,
  [string] $CMakeCache,
  [string] $CompileCommands,
  [string] $DispatchFile,
  [switch] $ListRecords,
  [switch] $ListResiduals,
  [switch] $ListUnmatchedSelectors
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-Input([string] $Path, [string] $Description) {
  if ([string]::IsNullOrEmpty($Path)) {
    return $null
  }
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "$Description does not exist: $Path"
  }
  return (Resolve-Path -LiteralPath $Path).Path
}

function New-Glob([string] $Pattern) {
  return [WildcardPattern]::new(
      $Pattern, [Management.Automation.WildcardOptions]::CultureInvariant)
}

try {
  $Executable = Resolve-Input $Executable 'executable'
  $Pdb = Resolve-Input $Pdb 'PDB'
  $PdbUtil = Resolve-Input $PdbUtil 'llvm-pdbutil'
  $SelectorList = Resolve-Input $SelectorList 'selector list'
  $CMakeCache = Resolve-Input $CMakeCache 'CMake cache'
  $CompileCommands = Resolve-Input $CompileCommands 'compile command database'
  $DispatchFile = Resolve-Input $DispatchFile 'dispatch file'

  if ((-not [string]::IsNullOrEmpty($Pdb)) -xor
      (-not [string]::IsNullOrEmpty($PdbUtil))) {
    throw '-Pdb and -PdbUtil must be supplied together'
  }

  # FileShare.Read makes an in-progress linker a hard error and never modifies
  # the image. Keep the handle open so all section and record reads use one
  # stable snapshot.
  $Stream = [IO.File]::Open($Executable, [IO.FileMode]::Open,
                            [IO.FileAccess]::Read, [IO.FileShare]::Read)
  $Reader = [IO.BinaryReader]::new($Stream)
  try {
    $Stream.Position = 0x3c
    $PEOffset = $Reader.ReadUInt32()
    $Stream.Position = $PEOffset
    if ($Reader.ReadUInt32() -ne 0x00004550) {
      throw 'input is not a PE/COFF image'
    }
    [void] $Reader.ReadUInt16()
    $SectionCount = $Reader.ReadUInt16()
    $Stream.Position = $PEOffset + 20
    $OptionalHeaderSize = $Reader.ReadUInt16()
    $OptionalHeader = $PEOffset + 24
    $Stream.Position = $OptionalHeader
    if ($Reader.ReadUInt16() -ne 0x20b) {
      throw 'input is not a 64-bit PE image'
    }
    $Stream.Position = $OptionalHeader + 24
    $ImageBase = $Reader.ReadUInt64()
    $Stream.Position = $OptionalHeader + $OptionalHeaderSize

    $Sections = [Collections.Generic.List[object]]::new()
    for ($I = 0; $I -lt $SectionCount; ++$I) {
      $Name = [Text.Encoding]::ASCII.GetString($Reader.ReadBytes(8)).
          TrimEnd([char] 0)
      $VirtualSize = $Reader.ReadUInt32()
      $RVA = $Reader.ReadUInt32()
      $RawSize = $Reader.ReadUInt32()
      $RawOffset = $Reader.ReadUInt32()
      $Sections.Add([pscustomobject]@{
        Name = $Name
        VirtualSize = [uint64] $VirtualSize
        RVA = [uint64] $RVA
        RawSize = [uint64] $RawSize
        RawOffset = [uint64] $RawOffset
      })
      $Stream.Position += 16
    }

    function Resolve-VA([uint64] $VA) {
      if ($VA -lt $ImageBase) {
        throw ('VA 0x{0:x} is below the image base' -f $VA)
      }
      $RVA = [uint64] ($VA - $ImageBase)
      foreach ($Section in $Sections) {
        $Delta = [int64] $RVA - [int64] $Section.RVA
        if ($Delta -ge 0 -and $Delta -lt $Section.RawSize) {
          return [int64] ($Section.RawOffset + $Delta)
        }
      }
      throw ('VA 0x{0:x} is not backed by image data' -f $VA)
    }

    function Read-ZString([uint64] $VA) {
      $Stream.Position = Resolve-VA $VA
      $Bytes = [Collections.Generic.List[byte]]::new()
      while ($Bytes.Count -lt 16384) {
        $Byte = $Reader.ReadByte()
        if ($Byte -eq 0) {
          return [Text.Encoding]::UTF8.GetString($Bytes.ToArray())
        }
        $Bytes.Add($Byte)
      }
      throw ('unterminated arena name at VA 0x{0:x}' -f $VA)
    }

    $ArenaSection = $Sections | Where-Object Name -CEQ '.llvma'
    if ($null -eq $ArenaSection) {
      throw 'image has no .llvma section'
    }
    if (($ArenaSection.VirtualSize % 8) -ne 0 -or
        $ArenaSection.VirtualSize -lt 16 -or
        $ArenaSection.VirtualSize -gt $ArenaSection.RawSize) {
      throw '.llvma has an invalid size'
    }

    $Stream.Position = $ArenaSection.RawOffset
    $Pointers = [Collections.Generic.List[uint64]]::new()
    for ($I = 0; $I -lt ($ArenaSection.VirtualSize / 8); ++$I) {
      $Pointers.Add($Reader.ReadUInt64())
    }
    if ($Pointers[0] -ne 0 -or $Pointers[$Pointers.Count - 1] -ne 0) {
      throw '.llvma start/end sentinels are not null'
    }

    $Records = [Collections.Generic.List[object]]::new()
    $SeenPointers = [Collections.Generic.HashSet[uint64]]::new()
    $PayloadBytes = [uint64] 0
    $AllocationBytes = [uint64] 0
    $MaxAlignment = [uint64] 0
    $TemplateCount = 0
    for ($I = 1; $I -lt $Pointers.Count - 1; ++$I) {
      $RecordVA = $Pointers[$I]
      if ($RecordVA -eq 0 -or -not $SeenPointers.Add($RecordVA)) {
        throw ('null or duplicate record pointer at .llvma slot {0}' -f $I)
      }
      $Stream.Position = Resolve-VA $RecordVA
      $Offset = $Reader.ReadUInt64()
      $Size = $Reader.ReadUInt64()
      $Alignment = $Reader.ReadUInt64()
      $TemplateVA = $Reader.ReadUInt64()
      $NameVA = $Reader.ReadUInt64()
      if ($Offset -ne [uint64]::MaxValue -or $Size -eq 0 -or
          $Alignment -eq 0 -or
          (($Alignment -band ($Alignment - 1)) -ne 0)) {
        throw ('invalid ABI fields in record at VA 0x{0:x}' -f $RecordVA)
      }
      if ($TemplateVA -ne 0) {
        [void] (Resolve-VA $TemplateVA)
        ++$TemplateCount
      }
      $Name = Read-ZString $NameVA
      $Remainder = $AllocationBytes % $Alignment
      if ($Remainder -ne 0) {
        $AllocationBytes += $Alignment - $Remainder
      }
      $AllocationBytes += $Size
      $PayloadBytes += $Size
      $MaxAlignment = [Math]::Max($MaxAlignment, $Alignment)
      $Records.Add([pscustomobject]@{
        Name = $Name
        Size = $Size
        Alignment = $Alignment
        Template = if ($TemplateVA -eq 0) { 0 } else { 1 }
        RecordVA = $RecordVA
      })
    }

    $LifecycleSection = $Sections | Where-Object Name -CEQ '.llvmi'
    $UniqueNames = @($Records.Name | Sort-Object -CaseSensitive -Unique)
    Write-Output ('IMAGE Path={0} ImageBase=0x{1:x} Sections={2}' -f
                  $Executable, $ImageBase, $SectionCount)
    Write-Output ('SECTION Name=.llvma RVA=0x{0:x} VirtualSize={1} RawSize={2}' -f
                  $ArenaSection.RVA, $ArenaSection.VirtualSize,
                  $ArenaSection.RawSize)
    if ($null -ne $LifecycleSection) {
      Write-Output ('SECTION Name=.llvmi RVA=0x{0:x} VirtualSize={1} RawSize={2}' -f
                    $LifecycleSection.RVA, $LifecycleSection.VirtualSize,
                    $LifecycleSection.RawSize)
    } else {
      Write-Output 'SECTION Name=.llvmi Missing=1'
    }
    Write-Output (('ARENA Records={0} UniquePointers={1} UniqueNames={2} ' +
                   'Templates={3} ZeroInit={4} PayloadBytes={5} ' +
                   'AllocationBytes={6} MaxAlignment={7} Invalid=0') -f
                  $Records.Count, $SeenPointers.Count, $UniqueNames.Count,
                  $TemplateCount, ($Records.Count - $TemplateCount),
                  $PayloadBytes, $AllocationBytes, $MaxAlignment)

    if ($ListRecords) {
      Write-Output "RECORD`tName`tSize`tAlignment`tTemplate`tRecordVA"
      $Records | Sort-Object Name, RecordVA | ForEach-Object {
        Write-Output ("RECORD`t{0}`t{1}`t{2}`t{3}`t0x{4:x}" -f
                      $_.Name, $_.Size, $_.Alignment, $_.Template, $_.RecordVA)
      }
    }
  } finally {
    $Reader.Dispose()
    $Stream.Dispose()
  }

  if (-not [string]::IsNullOrEmpty($CMakeCache)) {
    $CacheText = Get-Content -LiteralPath $CMakeCache
    $Enabled = @($CacheText | Where-Object {
        $_ -CEQ 'LLVM_DRIVER_PER_INVOCATION_GLOBALS:BOOL=ON' }).Count
    $CProbe = @($CacheText | Where-Object {
        $_ -CEQ 'LLVM_HAVE_PER_INVOCATION_GLOBALS_C:INTERNAL=TRUE' }).Count
    $CXXProbe = @($CacheText | Where-Object {
        $_ -CEQ 'LLVM_HAVE_PER_INVOCATION_GLOBALS_CXX:INTERNAL=TRUE' }).Count
    Write-Output ('CMAKE Enabled={0} CProbe={1} CXXProbe={2}' -f
                  $Enabled, $CProbe, $CXXProbe)
    if ($Enabled -ne 1 -or $CProbe -ne 1 -or $CXXProbe -ne 1) {
      throw 'per-invocation globals are not enabled by successful C/C++ probes'
    }
  }

  if (-not [string]::IsNullOrEmpty($CompileCommands)) {
    $ArenaCommands = 0
    $ListCommands = 0
    $BothCommands = 0
    $LifecycleIDs = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    Get-Content -LiteralPath $CompileCommands | ForEach-Object {
      $HasArena = $_ -match '-fstatic-arena=([^ \\"}]+)'
      if ($HasArena) {
        ++$ArenaCommands
        [void] $LifecycleIDs.Add($Matches[1])
      }
      $HasList = $_ -match '-fstatic-arena-list='
      if ($HasList) { ++$ListCommands }
      if ($HasArena -and $HasList) { ++$BothCommands }
    }
    Write-Output ('COMPILE_FLAGS Arena={0} List={1} Both={2} LifecycleIDs={3}' -f
                  $ArenaCommands, $ListCommands, $BothCommands,
                  $LifecycleIDs.Count)
    if ($ArenaCommands -eq 0 -or $ArenaCommands -ne $BothCommands) {
      throw 'arena compile commands are absent or missing their selector list'
    }
  }

  if (-not [string]::IsNullOrEmpty($DispatchFile)) {
    $DispatchCount = 0
    $DispatchOne = 0
    $ProcessClassificationCount = 0
    $ProcessInitializationCount = 0
    Get-Content -LiteralPath $DispatchFile | ForEach-Object {
      if ($_ -match
          '^LLVM_DRIVER_TOOL\("[^"]+",[ ]*[^,]+,[ ]*([01])(?:,[ ]*([01]))?\)$') {
        ++$DispatchCount
        if ($Matches[1] -eq '1') { ++$DispatchOne }
        if ($Matches.Count -gt 2 -and $Matches[2] -ne '') {
          ++$ProcessClassificationCount
          if ($Matches[2] -eq '1') { ++$ProcessInitializationCount }
        }
      }
    }
    $DispatchFormat = 'DISPATCH Entries={0} ArenaBitOne={1} Other={2} ' +
        'ProcessInitialization={3} Lightweight={4} Unclassified={5}'
    Write-Output ($DispatchFormat -f
                  $DispatchCount, $DispatchOne,
                  ($DispatchCount - $DispatchOne),
                  $ProcessInitializationCount,
                  ($ProcessClassificationCount -
                   $ProcessInitializationCount),
                  ($DispatchCount - $ProcessClassificationCount))
    if ($DispatchCount -eq 0 -or $DispatchCount -ne $DispatchOne) {
      throw 'one or more folded-driver dispatch entries are not arena-enabled'
    }
    if ($ProcessClassificationCount -ne $DispatchCount) {
      throw 'one or more folded-driver entries lack process initialization policy'
    }
  }

  $NameSelectors = @()
  $TypeSelectors = @()
  if (-not [string]::IsNullOrEmpty($SelectorList)) {
    Get-Content -LiteralPath $SelectorList | ForEach-Object {
      if ($_ -match '^name:(.+)$') { $NameSelectors += $Matches[1] }
      if ($_ -match '^type:(.+)$') { $TypeSelectors += $Matches[1] }
    }
    $Matched = [Collections.Generic.List[string]]::new()
    $Unmatched = [Collections.Generic.List[string]]::new()
    foreach ($Selector in $NameSelectors) {
      $Key = ($Selector -split '@')[-1]
      $Key = ($Key -split '::')[-1]
      $Glob = New-Glob "*$Key*"
      $Hit = $false
      foreach ($Name in $Records.Name) {
        if ($Glob.IsMatch($Name)) { $Hit = $true; break }
      }
      if ($Hit) { $Matched.Add($Selector) } else { $Unmatched.Add($Selector) }
    }
    Write-Output (('SELECTORS Names={0} Types={1} NameHeuristicMatched={2} ' +
                   'NameHeuristicUnmatched={3}') -f
                  $NameSelectors.Count, $TypeSelectors.Count, $Matched.Count,
                  $Unmatched.Count)
    if ($ListUnmatchedSelectors) {
      $Unmatched | Sort-Object -CaseSensitive | ForEach-Object {
        Write-Output "UNMATCHED_SELECTOR`t$_"
      }
    }
  }

  if (-not [string]::IsNullOrEmpty($Pdb)) {
    $Storage = [Collections.Generic.List[object]]::new()
    $Pending = $null
    & $PdbUtil dump -globals $Pdb | ForEach-Object {
      if ($_ -match '\| (S_(?:G|L)(?:DATA|THREAD)32).*`([^`]*)`') {
        if ($null -ne $Pending) { $Storage.Add($Pending) }
        $Pending = [pscustomobject]@{
          Kind = $Matches[1]
          Name = $Matches[2]
          Type = ''
        }
      } elseif ($null -ne $Pending -and
                $_ -match '^\s*type = .*\((.*)\)(?:,.*)?$') {
        $Pending.Type = $Matches[1]
        $Storage.Add($Pending)
        $Pending = $null
      }
    }
    if ($LASTEXITCODE -ne 0) {
      throw "llvm-pdbutil failed with exit code $LASTEXITCODE"
    }
    if ($null -ne $Pending) { $Storage.Add($Pending) }
    $PdbUniqueNames = @($Storage.Name | Sort-Object -CaseSensitive -Unique)
    Write-Output ('PDB_STORAGE Records={0} UniqueNames={1}' -f
                  $Storage.Count, $PdbUniqueNames.Count)

    if (-not [string]::IsNullOrEmpty($SelectorList)) {
      $Residuals = [Collections.Generic.List[object]]::new()
      $TypeGlobs = @($TypeSelectors | ForEach-Object {
          [pscustomobject]@{ Selector = $_; Glob = New-Glob $_ }
        })
      $NameMap = [Collections.Generic.Dictionary[
          string, Collections.Generic.List[string]]]::new(
              [StringComparer]::Ordinal)
      $NameGlobs = [Collections.Generic.List[object]]::new()
      $SourceQualifiedCount = 0
      foreach ($Selector in $NameSelectors) {
        # GSI records do not identify the defining source file. Do not turn a
        # common terminal name (for example ToolName) into a false residual for
        # a source-qualified selector.
        if ($Selector.Contains('@')) {
          ++$SourceQualifiedCount
          continue
        }
        $Candidate = ($Selector -split '@')[-1]
        if ($Candidate -match '[*?]') {
          $NameGlobs.Add([pscustomobject]@{
            Selector = $Selector; Glob = New-Glob $Candidate
          })
          continue
        }
        $SelectorsForName = $null
        if (-not $NameMap.TryGetValue($Candidate, [ref] $SelectorsForName)) {
          $SelectorsForName = [Collections.Generic.List[string]]::new()
          $NameMap.Add($Candidate, $SelectorsForName)
        }
        $SelectorsForName.Add($Selector)
      }
      foreach ($Item in $Storage) {
        foreach ($TypeGlob in $TypeGlobs) {
          $TypeBase = $Item.Type -replace '<.*$', ''
          if ($TypeGlob.Glob.IsMatch($TypeBase)) {
            $Residuals.Add([pscustomobject]@{
              Selector = "type:$($TypeGlob.Selector)"
              Name = $Item.Name
              Type = $Item.Type
            })
            break
          }
        }
        $SelectorsForName = $null
        if ($NameMap.TryGetValue($Item.Name, [ref] $SelectorsForName)) {
          foreach ($Selector in $SelectorsForName) {
            $Residuals.Add([pscustomobject]@{
              Selector = "name:$Selector"
              Name = $Item.Name
              Type = $Item.Type
            })
          }
        }
        foreach ($NameGlob in $NameGlobs) {
          if ($NameGlob.Glob.IsMatch($Item.Name)) {
            $Residuals.Add([pscustomobject]@{
              Selector = "name:$($NameGlob.Selector)"
              Name = $Item.Name
              Type = $Item.Type
            })
          }
        }
      }
      Write-Output (('PDB_SELECTOR_CANDIDATES Records={0} ' +
                     'UnattributableSourceSelectors={1}') -f
                    $Residuals.Count, $SourceQualifiedCount)
      if ($ListResiduals) {
        $Residuals | Sort-Object Selector, Name, Type | ForEach-Object {
          Write-Output ("CANDIDATE`t{0}`t{1}`t{2}" -f
                        $_.Selector, $_.Name, $_.Type)
        }
      }
    }
  }
} catch {
  [Console]::Error.WriteLine("ERROR: $($_.Exception.Message)")
  exit 1
}
