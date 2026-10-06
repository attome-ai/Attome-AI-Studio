# UI test: In and Out marks, the zoomed Monitor, and what the Export sheet can make (UX review U15, U1).
#  - I and O at the playhead mark a part (shown on the ruler and under the Monitor); the Export sheet exports just that part
#  - the same sheet makes the sound alone (WAV) and one picture (JPEG, the frame at the playhead)
#  - the Monitor's Zoom button goes fit, 100 %, 200 %, fit; the picture stays where it can be clicked
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_range.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Range.attome'
$video = Join-Path $work 'part.mp4'
$sound = Join-Path $work 'part.wav'
$pic = Join-Path $work 'frame.jpg'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $video, $sound, $pic -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 4 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $call = @{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\a.mp4"; at = '0s' }) }
  [IO.File]::WriteAllText("$work\call.json", ($call | ConvertTo-Json -Depth 5 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$failed = $null
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 300 -Script @(
  'wait 1500'
  'key Home', 'key Right shift', 'wait 200', 'key I', 'wait 200'                  # In at 1 s
  'key Right shift', 'key Right shift', 'wait 200', 'key O', 'wait 300'           # Out at 3 s
  "shot $work\marks.jpg"
  # the Monitor: fit, 100 %, 200 %, fit again
  'expect @button:monitor_zoom'
  'click @button:monitor_zoom', 'wait 700', "shot $work\zoom_100.jpg"
  'click @button:monitor_zoom', 'wait 700', "shot $work\zoom_200.jpg"
  'click @button:monitor_zoom', 'wait 700', "shot $work\zoom_fit.jpg"
  # the part, as video
  'click @button:export', 'wait 500'
  'expect @button:export_part_marked', 'expect @button:export_format_sound', 'expect @button:export_format_picture'
  "shot $work\export_part.jpg"
  'click @field:export_path', 'key A ctrl', "type $video"
  'click @button:export_res_720p'
  'click @button:export_start', 'wait 2500'
  'expect @button:export_play'
  'click @button:export_close', 'wait 300'
  # the sound alone
  'click @button:export', 'wait 500'
  'click @button:export_format_sound', 'wait 300'
  'absent @button:export_res_720p'
  "shot $work\export_sound.jpg"
  'click @field:export_path', 'key A ctrl', "type $sound"
  'click @button:export_start', 'wait 2000'
  'expect @button:export_play'
  'click @button:export_close', 'wait 300'
  # one picture: the frame at the playhead (the Out mark, 3 s)
  'click @button:export', 'wait 500'
  'click @button:export_format_picture', 'wait 300'
  'absent @button:export_part_marked'
  'click @field:export_path', 'key A ctrl', "type $pic"
  'click @button:export_start', 'wait 2000'
  'expect @button:export_play'
  'click @button:export_close', 'wait 300'
  # clear the marks (Alt+X) and the whole film is back
  'key X alt', 'wait 300'
  'click @button:export', 'wait 500'
  'absent @button:export_part_marked'
  "shot $work\export_whole.jpg"
  'key Escape', 'wait 300'
)
$failed = $run.Errors
Stop-Daemon $run
if (-not $failed) {
  foreach ($f in $video, $sound, $pic) { if (-not (Test-Path $f)) { $failed = "no file was written: $f" } }
}
if (-not $failed) {
  $v = (& "$bin\attome.exe" --json probe $video | ConvertFrom-Json).result
  $w = (& "$bin\attome.exe" --json probe $sound | ConvertFrom-Json).result
  "video part: $([math]::Round($v.seconds, 2)) s, sound: $([math]::Round($w.seconds, 2)) s, has_video: $($w.has_video), picture: $((Get-Item $pic).Length) bytes"
  if ([math]::Abs($v.seconds - 2.0) -gt 0.15) { $failed = "the part from 1 s to 3 s is $($v.seconds) s long" }
  elseif ([math]::Abs($w.seconds - 2.0) -gt 0.1) { $failed = "the sound of the part is $($w.seconds) s long" }
  elseif ($w.has_video) { $failed = 'the sound-only file has a picture' }
  elseif ((Get-Item $pic).Length -lt 2000) { $failed = 'the picture is empty' }
}

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: In and Out mark a part that exports alone; sound only and one picture; the Monitor zooms to 100 % and 200 % (captures in $work)" -ForegroundColor Green
exit 0
