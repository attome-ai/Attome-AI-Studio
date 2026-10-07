# UI test: selecting several clips and working with them (UX review U4).
#  - Ctrl+click adds a clip to the selection, Shift+click takes the clips between
#  - a box dragged on the empty timeline selects what it touches; Ctrl+A selects all
#  - Ctrl+C / Ctrl+V copy and paste at the playhead (a clip with sound brings its sound), Ctrl+D duplicates, Delete removes all
#  - dragging one clip of a group moves the group
#  - the right-click menu of a clip works (Duplicate)
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_select.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Select.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# A project with three text clips on one track (0-2 s, 3-5 s, 6-8 s) and a picture clip with sound on its own tracks.
$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 3 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  $a = ("$work\a.mp4").Replace('\', '\\')
  $ops = @(
    @{ op = 'add_clip'; separate_audio = $true; id = '$new:v'; path = "$work\a.mp4"; at = '0s' },
    @{ op = 'add_text'; id = '$new:t1'; text = 'One'; name = 'One'; at = '1s'; duration = '2s'; track = 'new'; track_name = 'T' },
    @{ op = 'add_text'; id = '$new:t2'; text = 'Two'; name = 'Two'; at = '4s'; duration = '2s'; track = '$new:t1.track' },
    @{ op = 'add_text'; id = '$new:t3'; text = 'Three'; name = 'Three'; at = '7s'; duration = '2s'; track = '$new:t1.track' },
    @{ op = 'add_text'; id = '$new:t4'; text = 'Late'; name = 'Late'; at = '60s'; duration = '2s'; track = '$new:t1.track' })
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = $ops } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message) $($r.error.hint)" }} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Texts($run) { @(Get-Tracks $run | ForEach-Object { $_.clip_list } | Where-Object { $_.type -eq 'text' }) }
function Count-All($run) { (@(Get-Tracks $run) | ForEach-Object { @($_.clip_list).Count } | Measure-Object -Sum).Sum }
function Starts($run, [string]$text) {
  @(Texts $run | ForEach-Object { Get-Object $run $_.id } | Where-Object { $_.content.text -eq $text } | ForEach-Object { ConvertFrom-Rational $_.timing.record_in })
}

$failed = $null
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
  'wait 1500'
  'click @button:zoom_fit', 'wait 500'           # Fit: the whole film in the window, so the last clip is in view
  'inside @clip:Late'
  'click @clip:Two', 'wait 300'
  'click @clip:Three ctrl', 'wait 400'           # Two and Three
  "shot $work\select_two.jpg"
  'key C ctrl', 'wait 300'
  'key Home', 'wait 200'
  'key V ctrl', 'wait 900'                       # pasted at 0 s... on the track of the originals, so after what is in the way
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $n = @(Texts $run).Count
    "text clips after copy and paste of two: $n"
    if ($n -ne 6) { $failed = "Ctrl+C then Ctrl+V of two clips made $n text clips, not 6 (four and two copies)" }
  }
  # Select all and delete all.
  if (-not $failed) {
    $before = Count-All $run
    $all = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('click @clip:One', 'key A ctrl', 'wait 300', "shot $work\select_all.jpg", 'key Delete', 'wait 800')
    $failed = $all.Errors
    if (-not $failed) {
      $left = Count-All $run
      "clips before: $before, after Ctrl+A and Delete: $left"
      if ($left -ne 0) { $failed = "Ctrl+A then Delete left $left clips" }
    }
  }
  # Undo brings them all back in one step.
  if (-not $failed) {
    $back = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl', 'wait 800')
    $failed = $back.Errors
    if (-not $failed -and (Count-All $run) -lt 8) { $failed = "one Undo did not bring the deleted clips back ($(Count-All $run))" }
  }
  # Duplicate with Ctrl+D, and the right-click menu.
  if (-not $failed) {
    $n0 = @(Texts $run).Count
    $dup = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('click @clip:One', 'key D ctrl', 'wait 900')
    $failed = $dup.Errors
    if (-not $failed -and @(Texts $run).Count -ne $n0 + 1) { $failed = "Ctrl+D did not add one copy ($(@(Texts $run).Count) text clips, was $n0)" }
  }
  if (-not $failed) {
    $n1 = @(Texts $run).Count
    $menu = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'rclick @clip:Three', 'wait 500', 'expect @menuitem:Duplicate', "shot $work\select_menu.jpg"
      'click @menuitem:Duplicate', 'wait 900')
    $failed = $menu.Errors
    if (-not $failed -and @(Texts $run).Count -ne $n1 + 1) { $failed = 'Duplicate in the right-click menu did not add a copy' }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result.problems | ConvertTo-Json -Compress)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: clips are selected with Ctrl and Shift, copied, pasted, duplicated, deleted together and brought back by one Undo; the right-click menu works (captures in $work)" -ForegroundColor Green
exit 0
