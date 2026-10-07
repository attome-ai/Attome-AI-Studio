# UI test: tracks (UX review 2: C1, A1, A5). The rows run from the top layer down (a title is above the video it covers, sound at
# the bottom); a track is renamed by a double click on its name, gets a menu on the right button, and is deleted with what is on
# it, which one Undo brings back; a sound track can be added. Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_tracks.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Tracks.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 4 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; separate_audio = $true; path = "$work\a.mp4"; at = '0s' },
        @{ op = 'add_text'; text = 'Hello'; name = 'Hello'; at = '0s'; duration = '2s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Names($run) { (@(Get-Tracks $run) | ForEach-Object { $_.name }) -join ',' }

$failed = $null
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'expect @track:Titles', 'expect @track:V1', 'expect @track:A1'
  'above @track:Titles @track:V1'      # the title is drawn over the video: its row is above
  'above @track:V1 @track:A1'          # sound at the bottom
  "shot $work\tracks_order.jpg"
  'dblclick @track:V1', 'wait 400', 'expect @field:track_name', 'type Main', 'key Enter', 'wait 700'
  'expect @track:Main'
  'rclick @track:Titles', 'wait 400', 'expect @menuitem:Rename', 'expect @menuitem:Delete_track_and_its_1_clip'
  "shot $work\tracks_menu.jpg"
  'click @menuitem:Add_an_audio_track', 'wait 700'
  'expect @track:A2'
  'rclick @track:Titles', 'wait 400', 'click @menuitem:Delete_track_and_its_1_clip', 'wait 800'
  'absent @track:Titles', 'absent @clip:Hello'
  "shot $work\tracks_deleted.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $names = Names $run
    "tracks after rename, add and delete: $names"
    if ($names -notmatch 'Main') { $failed = 'the track was not renamed' }
    elseif ($names -match 'Titles') { $failed = 'the Titles track was not deleted' }
    elseif ($names -notmatch 'A2') { $failed = 'no second sound track was added' }
  }
  if (-not $failed) { # one Undo brings the track and its clip back
    $undo = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl', 'wait 900', 'expect @track:Titles', 'expect @clip:Hello')
    $failed = $undo.Errors
  }
  if (-not $failed) { # the film's shape is changed from the Project card (nothing selected); Undo takes it back
    $shape = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Escape', 'wait 400', 'expect @button:shape_9x16', 'click @button:shape_9x16', 'wait 900', "shot $work\tracks_shape.jpg")
    $failed = $shape.Errors
    if (-not $failed) {
      $canvas = (Invoke-Attome $run --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].canvas
      "canvas after 9:16: $($canvas.width) x $($canvas.height)"
      if ($canvas.width -ne 1080 -or $canvas.height -ne 1920) { $failed = "the shape is $($canvas.width) x $($canvas.height), not 1080 x 1920" }
    }
    if (-not $failed) {
      $back = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl', 'wait 900')
      $failed = $back.Errors
      $canvas = (Invoke-Attome $run --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].canvas
      if (-not $failed -and $canvas.width -ne 1920) { $failed = "Undo left the shape at $($canvas.width) x $($canvas.height)" }
    }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result | ConvertTo-Json -Compress -Depth 4)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: rows run from the top layer down; a track is renamed, gets a menu, a sound track is added, and a deleted track comes back with Undo (captures in $work)" -ForegroundColor Green
exit 0
