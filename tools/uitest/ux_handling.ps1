# UI test: the handling details brought over from the ui-fixes branch.
#  - Sliders: Gain sticks at 0 dB when the knob comes near it; Scale is spread by ratio (the middle of 10%..400% is about 63%, not 205%);
#    Shift drags a tenth as fast.
#  - A trimmed end catches on a marker (and the playhead and other clips' edges), as a moved clip does; its linked sound follows.
#  - Esc during a drag lets it go without the edit.
#  - L plays, K stops.
#  - Ctrl+wheel zooms the timeline about the pointer: the time under it stays put (checked by the captures).
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_handling.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Handling.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\v.mp4" "--seconds 4 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $ops = @(@{ op = 'add_clip'; separate_audio = $true; path = "$work\v.mp4"; at = '0s' }, @{ op = 'add_marker'; at = '3s'; name = 'cut here' })
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = $ops } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Sec($r) { [double](ConvertFrom-Rational $r) }
function Both($run) {
  $out = @{}
  foreach ($t in @(Get-Tracks $run)) { foreach ($c in @($t.clip_list)) { $out[$t.kind] = Get-Object $run $c.id } }
  $out
}

$failed = $null
$run = Invoke-EditorScript -Project $proj -SelectFirstClip -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'slide @slider:gain 0.78', 'wait 800'      # 0.56 dB on a plain track: caught by 0 dB, 1.1 % of the track away
  "shot $work\gain_stuck.jpg"
  'slide @slider:scale 0.5', 'wait 800'      # by ratio: 10% x 40^0.5 = 63%
  'drag @slider:scale 20 0 shift', 'wait 800'  # from 63%, a tenth as fast: under 95% at any width from 76 points, where a plain drag gives 200% or more
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $b = Both $run
    $gain = $b.audio.audio.gain_db; $scale = $b.video.transform.scale
    "gain $gain dB, scale $($scale | ConvertTo-Json -Compress)"
    if ($null -ne $gain -and [double]$gain -ne 0) { $failed = "Gain set to 78 % of its track gave $gain dB, not 0 dB (the knob did not stick at 0)" }
    else {
      $s = [double]@($scale)[0]
      if ($s -lt 0.64 -or $s -gt 0.95) { $failed = "Scale after the middle and a Shift drag is $s, not between 0.64 and 0.95 (by ratio, then fine)" }
    }
  }
  $vid = (@(Get-Tracks $run) | Where-Object kind -eq 'video' | Select-Object -First 1).clip_list[0].id # the picture: its sound has the same name
  if (-not $failed) { # a trimmed end catches on the marker at 3 s: 85 points left of the end at 90 a second is 3.06 s without it
    $trim = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'wait 900', "click @clip:$vid", 'wait 300', "shot $work\hover_grips.jpg"
      "drag @clip:$vid@1.01,0.5 -85 0 hold", 'wait 400', "shot $work\trim_held.jpg", 'release', 'wait 900')
    $failed = $trim.Errors
    if (-not $failed) {
      $b = Both $run
      $d = Sec $b.video.timing.duration; $ds = Sec $b.audio.timing.duration
      "trimmed to $d s, its sound to $ds s"
      if ([math]::Abs($d - 3.0) -gt 0.001) { $failed = "the trimmed end is at $d s, not on the marker at 3 s" }
      elseif ([math]::Abs($ds - 3.0) -gt 0.001) { $failed = "the picture was trimmed to 3 s but its linked sound is $ds s" }
    }
  }
  if (-not $failed) { # Esc in the middle of a drag: nothing moves
    $esc = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'wait 900', "drag @clip:$vid 150 0 hold", 'wait 300', "shot $work\esc_held.jpg", 'key Escape', 'wait 300', 'release', 'wait 900', "shot $work\esc_after.jpg")
    $failed = $esc.Errors
    if (-not $failed) {
      $at = Sec (Both $run).video.timing.record_in
      "after Esc the clip starts at $at s"
      if ($at -ne 0) { $failed = "Esc did not cancel the drag: the clip moved to $at s" }
    }
  }
  if (-not $failed) { # L plays, K stops; Ctrl+wheel over the timeline zooms about the pointer
    $keys = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'wait 900', 'key L', 'wait 500', 'expect @transport:playing', 'key K', 'wait 500', 'absent @transport:playing'
      "shot $work\zoom_before.jpg", "wheel @clip:$vid@0.75,0.5 3 ctrl", 'wait 600', "shot $work\zoom_after.jpg")
    $failed = $keys.Errors
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result | ConvertTo-Json -Compress -Depth 4)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: sliders stick, spread by ratio and go fine with Shift; a trim catches on a marker; Esc lets a drag go; K and L (captures in $work)" -ForegroundColor Green
exit 0
