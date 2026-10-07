# UI test: Escape closes an open menu and nothing else; with no menu open it takes the selection away.
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_escape.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Escape.attome'
Remove-Item -LiteralPath $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\v.mp4" "--seconds 3 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\v.mp4"; at = '0s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$run = Invoke-EditorScript -Project $proj -SelectFirstClip -TimeoutSeconds 200 -Script @(
  'wait 1200', 'expect @slider:speed'
  'click @menu:Edit', 'wait 400', 'key Escape', 'wait 400'
  'expect @slider:speed'                 # the menu closed, the clip is still selected
  'key Escape', 'wait 400'
  'absent @slider:speed'                 # no menu: Escape deselects
)
Stop-Daemon $run
if ($run.Errors) { Write-Host "FAIL: $($run.Errors)" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Escape closes a menu without losing the selection, and deselects when no menu is open" -ForegroundColor Green
exit 0
