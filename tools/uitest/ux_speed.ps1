# UI test: speed (UX review 2: B1). The Clip card of a video has Speed: the 2x button makes the clip and its sound twice as fast and half
# as long; a typed 0.5 makes them slow and long; the clip shows its speed on the timeline; Undo takes it back. The rendered picture is
# checked by the unit tests (render: a clip with a speed ...). Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_speed.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Speed.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\v.mp4" "--seconds 4 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\v.mp4"; at = '0s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

# The sample file is 4.01 s long: the clip is too, so 2x gives 2.005 s.
function Sec($r) { [double](ConvertFrom-Rational $r) }
function Both($run) {
  $out = @{}
  foreach ($t in @(Get-Tracks $run)) { foreach ($c in @($t.clip_list)) { $o = Get-Object $run $c.id; $out[$t.kind] = $o } }
  $out
}

$failed = $null
$run = Invoke-EditorScript -Project $proj -SelectFirstClip -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'expect @slider:speed', 'expect @button:speed_2'
  'wide @clip:v 300'   # 4 s at 90 pixels a second: 360 wide
  'drag @slider:speed -30 0 hold', 'wait 500'                 # slower while held: the timeline draws it longer at once
  'wide @clip:v 500'
  "shot $work\speed_held.jpg"
  'release', 'wait 900'
  'key Z ctrl', 'wait 800'                                    # back to 1x for the steps below
  'click @button:speed_2', 'wait 900'
  "shot $work\speed_2x.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $b = Both $run
    "2x: picture $($b.video.timing.duration) s at $($b.video.timing.speed), sound $($b.audio.timing.duration) s at $($b.audio.timing.speed)"
    if ($b.video.timing.speed -ne 2 -or [math]::Abs((Sec $b.video.timing.duration) - 2.005) -gt 0.01) { $failed = 'the 2x button did not make the picture twice as fast and 2 s long' }
    elseif ($b.audio.timing.speed -ne 2 -or [math]::Abs((Sec $b.audio.timing.duration) - 2.005) -gt 0.01) { $failed = 'the sound did not follow the picture to 2x' }
  }
  if (-not $failed) { # at 2x the pitch is kept unless "Keep the pitch" is turned off: then like a tape, on the picture and its sound
    $kp = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('expect @check:keep_pitch', "shot $work\speed_pitch.jpg", 'click @check:keep_pitch', 'wait 800')
    $failed = $kp.Errors
    if (-not $failed) {
      $b = Both $run
      "keep_pitch after the switch: picture $($b.video.timing.keep_pitch), sound $($b.audio.timing.keep_pitch)"
      if ($b.video.timing.keep_pitch -ne $false -or $b.audio.timing.keep_pitch -ne $false) { $failed = 'turning Keep the pitch off did not reach the picture and its sound' }
    }
    if (-not $failed) {
      $back = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('click @check:keep_pitch', 'wait 800')
      $failed = $back.Errors
      if (-not $failed -and ((Both $run).audio.timing.PSObject.Properties.Name -contains 'keep_pitch')) { $failed = 'turning it on again did not take the field away' }
    }
  }
  if (-not $failed) { # a typed speed
    $typed = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('click @number:speed', 'wait 300', 'type 0.5', 'key Enter', 'wait 900', "shot $work\speed_half.jpg")
    $failed = $typed.Errors
    if (-not $failed) {
      $b = Both $run
      "0.5x: picture $($b.video.timing.duration) s"
      if ($b.video.timing.speed -ne 0.5 -or [math]::Abs((Sec $b.video.timing.duration) - 8.02) -gt 0.02) { $failed = "typing 0.5 gave $($b.video.timing.speed)x and $($b.video.timing.duration) s, not 0.5x and 8 s" }
    }
  }
  if (-not $failed) { # 1x takes the field away; Undo goes back to 0.5x
    $one = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('click @button:speed_1', 'wait 800')
    $failed = $one.Errors
    if (-not $failed) {
      $b = Both $run
      if ($b.video.timing.PSObject.Properties.Name -contains 'speed' -or [math]::Abs((Sec $b.video.timing.duration) - 4.01) -gt 0.01) { $failed = '1x did not bring the clip back to its own speed and 4 s' }
    }
    if (-not $failed) {
      $u = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl', 'wait 800')
      $failed = $u.Errors
      if (-not $failed -and (Both $run).video.timing.speed -ne 0.5) { $failed = 'Undo did not bring back 0.5x' }
    }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result | ConvertTo-Json -Compress -Depth 4)" }
  }
  if (-not $failed) { # the film renders, as long as the slow clip
    $out = Join-Path $work 'speed.mp4'
    Remove-Item $out -ErrorAction SilentlyContinue
    $ErrorActionPreference = 'Continue' # the render prints its progress on stderr
    $null = & "$bin\attome.exe" --endpoint $run.Endpoint render $proj --output $out --height 360 2>&1
    $ErrorActionPreference = 'Stop'
    $info = (& "$bin\attome.exe" --json probe $out | ConvertFrom-Json).result
    "render: $([math]::Round($info.seconds, 2)) s, audio $($info.has_audio)"
    if ([math]::Abs($info.seconds - 8.0) -gt 0.2 -or -not $info.has_audio) { $failed = "the render is $($info.seconds) s (audio $($info.has_audio)), not 8 s with sound" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Speed from the Clip card (buttons and a typed value) changes the clip and its sound, Undo takes it back, the film renders (captures in $work)" -ForegroundColor Green
exit 0
