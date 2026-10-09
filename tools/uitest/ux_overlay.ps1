# UI test: b-roll over the main video (UX review 4, P1 and P2). Over the rows is an empty lane: a clip dragged up into it, or a
# file from the Media panel let go there, goes on a new track on top of the others, drawn over the main video. A track's menu
# moves it up, down or to the top of its kind. The order is checked in the document (track_order runs from the bottom layer up);
# the captures show the b-roll, made small, over the talk. Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_overlay.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Overlay.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\talk.mp4" "--seconds 6 --height 360"
  New-Sample "$work\broll.mp4" "--seconds 3 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

# The picture tracks from the bottom layer up, each as "name:clip,clip".
function Layers($run) {
  (@(Get-Tracks $run) | Where-Object { $_.kind -ne 'audio' } | ForEach-Object { "$($_.name):" + (@($_.clip_list | ForEach-Object { $_.name }) -join ',') }) -join ' '
}

$failed = $null
# 1. The b-roll (after the talk on V1) dragged up into the lane over the rows, and left, so it plays over the end of the talk: a
# new track on top.
$run = Invoke-EditorScript -Project $proj -Import @("$work\talk.mp4", "$work\broll.mp4") -TimeoutSeconds 200 -Script @(
  'wait 1500'
  'drag @clip:broll -50% -100% hold', 'wait 300'
  "shot $work\clip_held_over_rows.jpg"
  'release', 'wait 900'
)
try {
  if ($run.Errors) { $failed = $run.Errors }
  elseif ((Layers $run) -ne 'V1:talk V2:broll') { $failed = "1. after the drag up the layers are: $(Layers $run)" }
} finally { if ($failed) { Stop-Daemon $run } }

# 2. The track menu: V2 down (under V1), then V2 to the top again.
if (-not $failed) {
  $r2 = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
    'wait 800'
    'rclick @track:V2', 'wait 400', "shot $work\track_menu.jpg", 'click @menuitem:Move_down', 'wait 800'
  )
  if ($r2.Errors) { $failed = $r2.Errors }
  elseif ((Layers $run) -ne 'V2:broll V1:talk') { $failed = "2. after Move down the layers are: $(Layers $run)" }
}
if (-not $failed) {
  $r3 = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('wait 800', 'rclick @track:V2', 'wait 400', 'click @menuitem:Move_to_the_top', 'wait 800')
  if ($r3.Errors) { $failed = $r3.Errors }
  elseif ((Layers $run) -ne 'V1:talk V2:broll') { $failed = "3. after Move to the top the layers are: $(Layers $run)" }
}

# 3. A file from the Media panel let go in the lane over the rows: another new track, on top of V2.
if (-not $failed) {
  $r4 = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
    'wait 800'
    'drag @media:broll.mp4 @clip:broll@0.5,-1.0 hold', 'wait 300'
    "shot $work\media_held_over_rows.jpg"
    'release', 'wait 900'
  )
  if ($r4.Errors) { $failed = $r4.Errors }
  elseif ((Layers $run) -notmatch '^V1:talk V2:broll V3:broll') { $failed = "4. after the media drop the layers are: $(Layers $run)" }
}

# The b-roll on V2, made small at a time both play: it is seen over the talk.
if (-not $failed) {
  $r5 = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
    'wait 800'
    'key Z ctrl', 'wait 600'                     # the media drop undone
    'rclick @clip:broll', 'wait 300', 'click @menuitem:Go_to_its_start', 'wait 400', 'key Right shift', 'wait 300', 'click @clip:broll', 'wait 300'
    'slide @slider:scale 0.3', 'wait 900'
    "shot $work\broll_over_talk.jpg"
  )
  if ($r5.Errors) { $failed = $r5.Errors }
}
Stop-Daemon $run

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a clip dragged or a file dropped over the rows goes on a new track on top, and the track menu moves tracks (captures in $work)" -ForegroundColor Green
exit 0
