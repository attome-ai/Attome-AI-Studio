#Requires -Version 5.1
<#
.SYNOPSIS
  Bring this checkout up to date: fetch the newest version, refresh
  dependencies, and rebuild.

.PARAMETER Channel  stable (newest tagged release, default) or dev (latest main).
.PARAMETER Force    Update even when you have local changes (they are kept in a git stash).
.PARAMETER Check    Only say whether an update is available.
#>
[CmdletBinding()]
param(
  [ValidateSet('stable', 'dev')][string]$Channel = 'stable',
  [switch]$Force,
  [switch]$Check
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

function Die($m) { Write-Host "error: $m" -ForegroundColor Red; exit 1 }
function Invoke-Git { & git @args; if ($LASTEXITCODE -ne 0) { Die "git $($args -join ' ') failed" } }

if (-not (Test-Path (Join-Path $Root '.git'))) { Die 'This folder is not a git checkout, so it cannot update itself. Clone the repository again.' }

Write-Host '==> Looking for updates' -ForegroundColor Cyan
Invoke-Git fetch --tags --prune origin

$target = 'origin/main'
if ($Channel -eq 'stable') {
  $tag = (& git tag --list 'v*' --sort=-v:refname | Select-Object -First 1)
  if ($tag) { $target = $tag } else { Write-Host 'No tagged release yet; following main.' -ForegroundColor Yellow }
}

$current = (& git rev-parse HEAD).Trim()
$new = (& git rev-parse "$target^{commit}").Trim()

if ($current -eq $new) {
  Write-Host "Already up to date ($target)." -ForegroundColor Green
  if ($Check) { exit 0 }
} else {
  Write-Host "Update available: $target" -ForegroundColor Green
  Write-Host 'What changed:'
  & git log --oneline --no-decorate -n 20 "$current..$new"
  if ($Check) { exit 10 }
}

$dirty = (& git status --porcelain)
if ($dirty -and -not $Force) {
  Die 'You have local changes. Commit or stash them, or run update.cmd -Force (they will be stashed, not lost).'
}
if ($dirty -and $Force) { Invoke-Git stash push -u -m "attome-update $(Get-Date -Format s)" }

if ($current -ne $new) {
  if ($Channel -eq 'stable' -and $target -ne 'origin/main') {
    Invoke-Git checkout --detach $target
  } else {
    Invoke-Git checkout main
    Invoke-Git merge --ff-only origin/main
  }
}

Write-Host '==> Refreshing dependencies and rebuilding' -ForegroundColor Cyan
& (Join-Path $PSScriptRoot 'setup.ps1') -Yes
if ($LASTEXITCODE -ne 0) { Die 'Setup failed after the update. Run setup.cmd to see the details.' }
Write-Host 'Updated.' -ForegroundColor Green
