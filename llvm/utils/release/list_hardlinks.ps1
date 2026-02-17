param(
    [string]$FirstArgument
)

$files = Get-ChildItem $FirstArgument\*.exe

# Group by file index (hard links share the same index)
$groups = $files | Group-Object {
    (fsutil file queryfileid $_.FullName).Trim()
}

# Display each group
foreach ($group in $groups | Sort-Object { -$_.Group[0].Length }) {
    $size = $group.Group[0].Length
    $sizeMB = "{0,8:N1} MB" -f ($size / 1MB)

    if ($group.Count -gt 1) {
        $names = ($group.Group | Sort-Object Name | ForEach-Object { $_.Name }) -join ', '
        Write-Host "$sizeMB  [HARDLINKED x$($group.Count)] $names" -ForegroundColor Cyan
    } else {
        Write-Host "$sizeMB  $($group.Group[0].Name)"
    }
}

# Summary
$reported = ($files | Measure-Object -Property Length -Sum).Sum
$actual = ($groups | ForEach-Object { $_.Group[0].Length } | Measure-Object -Sum).Sum

Write-Host ""
Write-Host "Files:         $($files.Count) ($($groups.Count) unique)"
"Reported size: {0:N2} GB" -f ($reported / 1GB)
"Actual size:   {0:N2} GB" -f ($actual / 1GB)
"Savings:       {0:N2} GB ({1:P0})" -f (($reported - $actual) / 1GB), (1 - $actual / $reported)
