# UI test: a clip with no media before its start cannot take a dissolve; the Transition card says what is missing, Make
# room trims it and moves the later clips up, and the dissolve can then be added. Virtual input only (uitest.psm1).
# Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\make_room.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP 'attome-uitest'
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'MakeRoom.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# Three 3-second clips, each from the start of a 6-second file: a has 3 s after its out point, b has nothing before its
# in point (it starts at 0 of its file), c follows b.
$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" sample "$work\a.mp4" --seconds 6 --height 540 | Out-Null
  & "$bin\attome.exe" sample "$work\b.mp4" --seconds 6 --height 540 | Out-Null
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $seq = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].id
  $a = ("$work\a.mp4").Replace('\', '\\')
  $b = ("$work\b.mp4").Replace('\', '\\')
  @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"a.mp4","timing":{"record_in":"0","duration":"3","source_in":"0"},"media_ref":{"type":"file","path":"$a","duration":"6","width":960,"height":540}}},
 {"op":"add","path":"`$new:v1/clips/`$new:b","value":{"name":"b.mp4","timing":{"record_in":"3","duration":"3","source_in":"0"},"media_ref":{"type":"file","path":"$b","duration":"6","width":960,"height":540}}},
 {"op":"add","path":"`$new:v1/clips/`$new:c","value":{"name":"c.mp4","timing":{"record_in":"6","duration":"3","source_in":"0"},"media_ref":{"type":"file","path":"$b","duration":"6","width":960,"height":540}}}
]}
"@ | Set-Content "$work\pr.json" -Encoding utf8
  & "$bin\attome.exe" patch $proj "$work\pr.json" | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Get-Timing($run, [string]$name) {
  $track = @(Get-Tracks $run) | Where-Object { $_.name -eq 'V1' }
  $id = ($track.clip_list | Where-Object { $_.name -eq $name }).id
  (Get-Object $run $id).timing
}

# The first clip is selected; its card offers the dissolve. With a 1 s dissolve the next clip lacks 0.5 s before its start.
$run = Invoke-EditorScript -Project $proj -SelectFirstClip -Script @(
  'wait 500'
  "shot $work\make_room_offer.jpg"
  'click @button:make_room'
  'wait 500'
  "shot $work\make_room_done.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $b = Get-Timing $run 'b.mp4'
    $c = Get-Timing $run 'c.mp4'
    $a = Get-Timing $run 'a.mp4'
    "a: $($a.record_in) +$($a.duration) from $($a.source_in)   b: $($b.record_in) +$($b.duration) from $($b.source_in)   c: $($c.record_in)"
    if ($a.duration -ne '3') { $failed = "the first clip changed: it had media to spare ($($a.duration))" }
    elseif ($b.record_in -ne '3') { $failed = "the second clip no longer meets the first (starts at $($b.record_in))" }
    elseif ($b.source_in -ne '1/2' -or $b.duration -ne '5/2') { $failed = "the second clip was not trimmed by half a second at its start ($($b.source_in), $($b.duration))" }
    elseif ($c.record_in -ne '11/2') { $failed = "the third clip did not move up by half a second ($($c.record_in))" }
  }
  if (-not $failed) { # now it fits: the dissolve can be added from the same card
    $again = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('click @button:add_dissolve')
    $failed = $again.Errors
    if (-not $failed) {
      $track = Get-Object $run (@(Get-Tracks $run) | Where-Object { $_.name -eq 'V1' }).id
      $t = @($track.transitions.PSObject.Properties | ForEach-Object { $_.Value })
      if ($t.Count -ne 1 -or $t[0].type -ne 'attome.dissolve') { $failed = 'the dissolve was not added after making room' }
    }
  }
  if (-not $failed) { # one undo takes the dissolve away, a second undoes the trimming
    $undo = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl', 'key Z ctrl')
    $failed = $undo.Errors
    if (-not $failed) {
      $b = Get-Timing $run 'b.mp4'
      $c = Get-Timing $run 'c.mp4'
      if ($b.source_in -ne '0' -or $b.duration -ne '3' -or $c.record_in -ne '6') { $failed = "undo did not restore the clips (b from $($b.source_in) +$($b.duration), c at $($c.record_in))" }
    }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Make room trimmed the next clip and moved the later ones up, the dissolve was added, two undos restored it (captures in $work)" -ForegroundColor Green
exit 0
