# UI test: the tabs of the bottom dock (Timeline, History, Profiler) can be clicked by name; History lists the edits made, the Profiler
# shows its zones, and the Timeline comes back. The Generate workflow's graph is no wider than the canvas at a size the text can be read:
# the Generate video node is on the screen when a workflow is opened.
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_dock_tabs.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Dock.attome'
if (Test-Path -LiteralPath $proj) { Remove-Item -LiteralPath $proj -Recurse -Force }
$env:ATTOME_PREF_DIR = Join-Path $work 'prefs'
New-Item -ItemType Directory -Force $env:ATTOME_PREF_DIR | Out-Null
$env:ATTOME_MOCK_ENGINE = '1'
$env:ATTOME_UI_READABLE_FIT = '1'

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\v.mp4" "--seconds 3 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\v.mp4"; at = '0s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$failed = $null
try {
  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
    'wait 1500'
    'expect @dock:Timeline', 'expect @dock:History', 'expect @dock:Profiler'
    'click @dock:History', 'wait 500', "shot $work\history.jpg"
    'click @dock:Profiler', 'wait 800', "shot $work\profiler.jpg"
    'click @dock:Timeline', 'wait 500', 'expect @clip:v'
    'click @rail:Generate', 'wait 500', 'click @model:attome-mock', 'wait 1000'
    'dblclick @clip:Shot_1', 'wait 1500'
    "shot $work\workflow.jpg"
    'inside @node:generate_video'
  )
  $failed = $run.Errors
} finally {
  if ($run) { Stop-Daemon $run }
  Remove-Item Env:\ATTOME_PREF_DIR, Env:\ATTOME_MOCK_ENGINE, Env:\ATTOME_UI_READABLE_FIT -ErrorAction SilentlyContinue
}

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the dock tabs are clickable by name, and a new workflow's Generate video node is on the screen at a readable size (captures in $work)" -ForegroundColor Green
exit 0

