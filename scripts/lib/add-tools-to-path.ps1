# The Windows half of scripts/add-tools-to-path.sh, which says what this is
# for and passes the folder in AUTANA_TOOLS_ROOT and --check in
# AUTANA_CHECK_ONLY.
$ErrorActionPreference = 'Stop'

$root = $env:AUTANA_TOOLS_ROOT.TrimEnd('\')
$checkOnly = $env:AUTANA_CHECK_ONLY -eq '1'

function Same-Folder($a, $b) {
    return $a.TrimEnd('\') -ieq $b.TrimEnd('\')
}

$user = [Environment]::GetEnvironmentVariable('Path', 'User')
if ($null -eq $user) { $user = '' }
$entries = @($user -split ';' | Where-Object { $_ -ne '' })
$present = @($entries | Where-Object { Same-Folder $_ $root }).Count -gt 0

if ($present) {
    Write-Output "already on the user PATH: $root"
} elseif ($checkOnly) {
    Write-Output "NOT on the user PATH: $root"
    exit 1
} else {
    $updated = (@($entries) + $root) -join ';'
    [Environment]::SetEnvironmentVariable('Path', $updated, 'User')
    Write-Output "added to the user PATH: $root"
}

# What a terminal opened from now on gets: the machine's PATH, then the user's.
$machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
$fresh = [Environment]::GetEnvironmentVariable('Path', 'User')
$env:Path = "$machine;$fresh"

$found = @(Get-Command autana -All -ErrorAction SilentlyContinue | ForEach-Object { $_.Source })
if ($found.Count -eq 0) {
    Write-Output 'a new terminal would NOT find autana'
    exit 1
}
Write-Output "a new terminal finds: $($found[0])"
if (-not (Same-Folder (Split-Path $found[0]) $root)) {
    Write-Output "but that is not this folder's - another autana is earlier on the PATH:"
    $found | ForEach-Object { Write-Output "  $_" }
    exit 1
}

& autana --help | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Output "autana was found but exited $LASTEXITCODE"
    exit 1
}
Write-Output 'and it runs.'
