# UI test: the first minutes with a short (UX review 3, R5 R6 R7 R9 R10).
#  - two videos and music opened with the editor: the toast names every track that took a clip ("to V1 and A1"), and the
#    film is shown from its start with the first clip selected
#  - S at a clip's first frame says why nothing was split, as a note, not as an error
#  - Undo says what it took back ("Undo: Delete clip")
#  - a right click on the empty space under the tracks opens the timeline's menu
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_review3.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Short.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\beach.mp4" "--seconds 6 --height 360"
  New-Sample "$work\city.mp4" "--seconds 5 --height 360"
  New-Sample "$work\music.wav" "--seconds 20"
  & "$bin\attome.exe" new $proj --rate 30 --canvas 1080x1920 | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$run = Invoke-EditorScript -Project $proj -Import @("$work\beach.mp4", "$work\city.mp4", "$work\music.wav") -TimeoutSeconds 200 -Script @(
  'wait 2500'
  'expect @toast:Add_3_clips_to_V1_and_A1'
  "shot $work\imported.jpg"
  'key S', 'wait 500'                      # the playhead is at the first clip's start, and it is selected
  'expect @toast:The_playhead_is_at_the_c'
  'key Delete', 'wait 700'                 # the selected clip: the first one
  'key Z ctrl', 'wait 700'
  'expect @toast:Undo:_Delete_clip'
  "shot $work\undone.jpg"
  'rclick 900,880', 'wait 500'             # under the two rows
  'expect @menuitem:Paste_here'
  "shot $work\menu_below.jpg"
  'key Escape', 'wait 300'
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $tracks = @(Get-Tracks $run)
    $v1 = $tracks | Where-Object { $_.name -eq 'V1' }
    $names = @($v1.clip_list | ForEach-Object { $_.name }) -join ','
    if ($names -ne 'beach,city') { $failed = "V1 holds '$names' after the delete was undone, not beach,city" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the import says where the clips went, a split at a clip's edge is a note, Undo names the step, the space under the tracks has a menu (captures in $work)" -ForegroundColor Green
exit 0
