# UI test: where a drop makes a new track (UX review 2, after the rows were turned to run from the top layer down).
#  - a title dropped on a title that is in the way goes on a new track over all the others (the top row), not under the video
#  - a video dropped on the lane below the last row makes the bottom picture layer, named with the next free name
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_drop_tracks.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Drop.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\v.mp4" "--seconds 4 --height 360"
  New-Sample "$work\w.mp4" "--seconds 2 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\v.mp4"; at = '0s' },
        @{ op = 'add_text'; text = 'Hello'; name = 'Hello'; at = '0s'; duration = '3s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

# The tracks in the document's order (the bottom layer first), each with the names of its clips.
function Layers($run) { @(Get-Tracks $run) | ForEach-Object { $names = @($_.clip_list | ForEach-Object { $_.name }) -join '+'; "$($_.name)[$($_.kind)]:$names" } }

$failed = $null
$run = Invoke-EditorScript -Project $proj -Import @("$work\w.mp4") -TimeoutSeconds 240 -Script @(
  'wait 1500'
  'key Z ctrl', 'wait 800'                       # the import put w.mp4 on the timeline: take it off again, the card stays
  'click @rail:Text', 'wait 300'
  'drag @style:Title @clip:Hello hold', 'wait 200', "shot $work\drop_title_held.jpg", 'release', 'wait 900'
  'click @rail:Media', 'wait 300'
  'drag @media:w.mp4 @track:A1@3,1.6 hold', 'wait 200', "shot $work\drop_lane_held.jpg", 'release', 'wait 1200'
  "shot $work\drop_after.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $layers = @(Layers $run)
    "layers, bottom first: $($layers -join '  |  ')"
    $pictures = @($layers | Where-Object { $_ -match '\[video\]' })
    if ($pictures[-1] -notmatch ':Title') { $failed = "the dropped title is not on the top layer: $($pictures[-1])" }
    elseif ($pictures[0] -notmatch ':w$') { $failed = "the video dropped below the rows is not the bottom picture layer: $($pictures[0])" }
    elseif (@($layers | ForEach-Object { ($_ -split '\[')[0] } | Group-Object | Where-Object { $_.Count -gt 1 }).Count) { $failed = 'two tracks have one name' }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result | ConvertTo-Json -Compress -Depth 4)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a title with no room goes on a new top track; a video on the lane below makes the bottom picture layer (captures in $work)" -ForegroundColor Green
exit 0
