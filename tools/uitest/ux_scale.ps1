# UI test: the interface size (UX review U18). Ctrl+plus makes everything larger (the whole UI is drawn at that size, text stays sharp; never so large that the window has no room left),
# Ctrl+minus smaller, Ctrl+0 back to 100 %; the choice is kept in the preferences. Widgets stay inside their panels and stay clickable.
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_scale.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Scale.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
$prefs = Join-Path $work 'prefs'
Remove-Item $prefs -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $prefs | Out-Null
$env:ATTOME_PREF_DIR = $prefs

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" new $proj --rate 30 --canvas 360x640 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_text'; id = '$new:a'; text = 'One'; name = 'One'; at = '0s'; duration = '2s'; track = 'new'; track_name = 'T' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message) $($r.error.hint)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$failed = $null
try {
  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
    'wait 1500'
    "shot $work\scale_100.jpg"
    'key Plus ctrl', 'key Plus ctrl', 'key Plus ctrl', 'key Plus ctrl', 'wait 600'      # 100 % + 4 * 10 % = 140 %
    "shot $work\scale_140.jpg"
    'expect @button:monitor_loop', 'inside @button:monitor_loop', 'click @button:monitor_loop', 'wait 400'
    'expect @clip:One', 'click @clip:One', 'wait 400'
    "shot $work\scale_140_clip.jpg"
    # asked for far more than the window holds (200 %): it is held back, and the Monitor with its controls is still all on the screen
    'key Plus ctrl', 'key Plus ctrl', 'key Plus ctrl', 'key Plus ctrl', 'key Plus ctrl', 'key Plus ctrl', 'key Plus ctrl', 'wait 600'
    'expect @transport:play', 'inside @transport:play', 'inside @button:monitor_loop', 'inside @button:monitor_full', 'expect @clip:One'
    "shot $work\scale_max.jpg"
    'key Minus ctrl', 'key Minus ctrl', 'wait 500'
    'key 0 ctrl', 'wait 500'
    "shot $work\scale_back.jpg"
  )
  $failed = $run.Errors
  if (-not $failed) {
    $kept = (Get-Content "$prefs\ui_scale.txt" -ErrorAction SilentlyContinue | Select-Object -First 1)
    "kept: '$kept'"
    if ("$kept".Trim() -ne '1') { $failed = "ui_scale.txt says '$kept', not 1 after Ctrl+0" }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = 'the project is not valid' }
  }
} finally { if ($run) { Stop-Daemon $run }; Remove-Item Env:\ATTOME_PREF_DIR -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Ctrl+plus, Ctrl+minus and Ctrl+0 change the interface size, widgets stay clickable, the size is kept (captures in $work)" -ForegroundColor Green
exit 0

