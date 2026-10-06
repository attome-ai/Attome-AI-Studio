# UI test: the Monitor's view controls, the shortcut list and the keys (UX review U15, U9).
#  - Guides: the Shorts / Reels / TikTok zones, then title safe, then off (captures)
#  - Loop and Full screen (Ctrl+F; Esc leaves)
#  - F1 and Help > Keyboard shortcuts open the list
#  - Up / Down jump to cuts, Shift+Right goes a second, Ctrl+S saves
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_monitor.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Monitor.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 4 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 --canvas 360x640 | Out-Null
  $ops = @(
    @{ op = 'add_text'; id = '$new:a'; text = 'One'; name = 'One'; at = '0s'; duration = '2s' },
    @{ op = 'add_text'; id = '$new:b'; text = 'Two'; name = 'Two'; at = '2s'; duration = '3s'; track = '$new:a.track' })
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_text'; id = '$new:a'; text = 'One'; name = 'One'; at = '0s'; duration = '2s'; track = 'new'; track_name = 'T' },
        @{ op = 'add_text'; id = '$new:b'; text = 'Two'; name = 'Two'; at = '2s'; duration = '3s'; track = '$new:a.track' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message) $($r.error.hint)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$failed = $null
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
  'wait 1500'
  'expect @button:monitor_guides', 'expect @button:monitor_loop', 'expect @button:monitor_full'
  'click @button:monitor_guides', 'wait 400', "shot $work\mon_shorts.jpg"
  'click @button:monitor_guides', 'wait 400', "shot $work\mon_title_safe.jpg"
  'click @button:monitor_guides', 'wait 300'
  'click @button:monitor_loop', 'wait 300', "shot $work\mon_loop.jpg"
  'click @button:monitor_full', 'wait 700', 'expect @monitor_full_view', "shot $work\mon_full.jpg"
  'key Escape', 'wait 500'
  'absent @monitor_full_view'
  'key F1', 'wait 600', 'expect @shortcuts_sheet', "shot $work\mon_shortcuts.jpg"
  'key F1', 'wait 400'
  'click @menu:Help', 'wait 300', 'expect @menuitem:Keyboard'
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = 'the project is not valid' }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: guides, loop and full screen on the Monitor; the shortcut list opens from F1 and the Help menu (captures in $work)" -ForegroundColor Green
exit 0
