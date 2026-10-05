# UI test: a media file dragged from the Media panel onto the timeline, with its linked sound. Let go on the right half
# of a clip it goes after it; on the left half before it, and the clips in the way slide right with their sound. Then a
# linked clip is moved in front of the others on the timeline. Nothing may overlap on either track, and every picture
# must still start with its sound. Virtual input only.
#   .\tools\uitest\media_drop.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Media.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 4 --height 540"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

# The clips of the picture track and of the audio track, by start. Throws when clips of a track overlap or a picture and
# its sound do not start together.
function Get-Pairs($run) {
  $out = @{ video = @(); audio = @() }
  foreach ($t in (Get-Tracks $run)) {
    foreach ($c in $t.clip_list) {
      $o = Get-Object $run $c.id
      $at = ConvertFrom-Rational $o.timing.record_in
      $out[$t.kind] += [pscustomobject]@{ Id = $c.id; At = [math]::Round($at, 3); End = [math]::Round($at + (ConvertFrom-Rational $o.timing.duration), 3); Link = $o.link_group }
    }
  }
  foreach ($kind in 'video', 'audio') {
    $out[$kind] = @($out[$kind] | Sort-Object At)
    for ($i = 1; $i -lt $out[$kind].Count; ++$i) {
      if ($out[$kind][$i].At -lt $out[$kind][$i - 1].End - 0.0005) { throw "two $kind clips overlap: one ends at $($out[$kind][$i - 1].End) s, the next starts at $($out[$kind][$i].At) s" }
    }
  }
  foreach ($v in $out.video) {
    $s = $out.audio | Where-Object { $_.Link -and $_.Link -eq $v.Link }
    if (-not $s) { throw "the picture at $($v.At) s has no linked sound" }
    if ($s.At -ne $v.At) { throw "the picture at $($v.At) s has its sound at $($s.At) s" }
  }
  $out
}

# The pictures follow each other without a gap (the sample is a little longer than 4 s, so a clip starts on the first
# whole frame after the one before).
function Packed($pairs, [int]$count) {
  if ($pairs.video.Count -ne $count) { return "there are $($pairs.video.Count) picture clips, expected $count" }
  if ($pairs.video[0].At -ne 0) { return "the first clip starts at $($pairs.video[0].At) s, not at 0" }
  for ($i = 1; $i -lt $count; ++$i) {
    if ($pairs.video[$i].At - $pairs.video[$i - 1].End -gt 0.04) { return "a gap opened before the clip at $($pairs.video[$i].At) s" }
  }
  $null
}

function Starts($pairs) { "pictures at $(($pairs.video | ForEach-Object { $_.At }) -join ', ')   sounds at $(($pairs.audio | ForEach-Object { $_.At }) -join ', ')" }

$run = Invoke-EditorScript -Project $proj -Import "$work\a.mp4" -Script @('wait 500')
$failed = $run.Errors
try {
  if (-not $failed) {
    $first = (Get-Pairs $run).video[0].Id
    # After the clip: its right half.
    $after = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      "drag @media:a.mp4 @clip:$first@0.75,0.5 hold"
      'wait 300'
      "shot $work\media_after_held.jpg"
      'release'
      'wait 800'
    )
    $failed = $after.Errors
    if (-not $failed) {
      try { $p = Get-Pairs $run; "1. dropped on the right half: $(Starts $p)" } catch { $failed = "1. $_" }
      if (-not $failed -and (Packed $p 2)) { $failed = "1. $(Packed $p 2)" }
      if (-not $failed -and $p.video[0].Id -ne $first) { $failed = '1. the new clip is not after the first one' }
    }
  }
  if (-not $failed) { # before the clip: its left half; both clips and their sounds slide four seconds right
    $before = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      "drag @media:a.mp4 @clip:$first@0.25,0.5 hold"
      'wait 300'
      "shot $work\media_before_held.jpg"
      'release'
      'wait 800'
    )
    $failed = $before.Errors
    if (-not $failed) {
      try { $p = Get-Pairs $run; "2. dropped on the left half: $(Starts $p)" } catch { $failed = "2. $_" }
      if (-not $failed -and (Packed $p 3)) { $failed = "2. $(Packed $p 3)" }
      if (-not $failed -and $p.video[1].Id -ne $first) { $failed = '2. the clip that was first is not second now' }
    }
  }
  if (-not $failed) { # a linked clip of the timeline moved in front of the others
    $last = $p.video[2].Id
    $move = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      "drag @clip:$last @clip:$($p.video[0].Id)@0.25,0.5 hold"
      'wait 300'
      "shot $work\media_move_held.jpg"
      'release'
      'wait 800'
      "shot $work\media_move_done.jpg"
    )
    $failed = $move.Errors
    if (-not $failed) {
      try { $p = Get-Pairs $run; "3. the last clip moved to the front: $(Starts $p)" } catch { $failed = "3. $_" }
      if (-not $failed -and $p.video[0].Id -ne $last) { $failed = '3. the moved clip is not first' }
      if (-not $failed -and (Packed $p 3)) { $failed = "3. $(Packed $p 3)" }
    }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: media dropped before and after a clip, and a linked clip moved, with every sound still under its picture (captures in $work)" -ForegroundColor Green
exit 0
