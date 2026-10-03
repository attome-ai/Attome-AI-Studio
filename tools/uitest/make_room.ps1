# UI test: a clip with no media before its start cannot take a dissolve; the Transition card says what is missing, Make
# room trims it and moves the later clips up (the clips on the Titles track too, unless it is switched off), and the
# dissolve can then be added. Virtual input only (uitest.psm1). Needs a build (build\win-msvc-release\bin).
# Exit code 0 = pass.
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
# in point (it starts at 0 of its file), c follows b. A Titles track has one title across the cut (2 s to 4 s) and one
# after it (6 s to 7 s).
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
 {"op":"add","path":"`$new:v1/clips/`$new:c","value":{"name":"c.mp4","timing":{"record_in":"6","duration":"3","source_in":"0"},"media_ref":{"type":"file","path":"$b","duration":"6","width":960,"height":540}}},
 {"op":"add","path":"$seq/tracks/`$new:t","value":{"kind":"video","name":"Titles"}},
 {"op":"add","path":"`$new:t/clips/`$new:across","value":{"name":"across","timing":{"record_in":"2","duration":"2","source_in":"0"},"media_ref":{"type":"text"},"content":{"text":"Across the cut","size":0.08}}},
 {"op":"add","path":"`$new:t/clips/`$new:later","value":{"name":"later","timing":{"record_in":"6","duration":"1","source_in":"0"},"media_ref":{"type":"text"},"content":{"text":"Later","size":0.08}}}
]}
"@ | Set-Content "$work\pr.json" -Encoding utf8
  & "$bin\attome.exe" patch $proj "$work\pr.json" | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Get-Timing($run, [string]$track, [string]$name) {
  $t = @(Get-Tracks $run) | Where-Object { $_.name -eq $track }
  $id = ($t.clip_list | Where-Object { $_.name -eq $name }).id
  (Get-Object $run $id).timing
}
function Show($run) {
  $b = Get-Timing $run 'V1' 'b.mp4'; $c = Get-Timing $run 'V1' 'c.mp4'
  $x = Get-Timing $run 'Titles' 'across'; $l = Get-Timing $run 'Titles' 'later'
  "b: $($b.record_in) +$($b.duration) from $($b.source_in)  c: $($c.record_in)   across: $($x.record_in) +$($x.duration)  later: $($l.record_in)"
}

$failed = $null
# 1. Titles switched off: the picture track makes room, the titles stay where they were.
$run = Invoke-EditorScript -Project $proj -SelectFirstClip -Script @(
  'expect @check:ripple_Titles'
  'click @check:ripple_Titles'
  'click @button:make_room'
  'wait 500'
)
$failed = $run.Errors
try {
  if (-not $failed) {
    Show $run
    $b = Get-Timing $run 'V1' 'b.mp4'; $c = Get-Timing $run 'V1' 'c.mp4'
    $x = Get-Timing $run 'Titles' 'across'; $l = Get-Timing $run 'Titles' 'later'
    if ($b.source_in -ne '1/2' -or $c.record_in -ne '11/2') { $failed = 'Make room did not trim the next clip and move the third up' }
    elseif ($x.duration -ne '2' -or $l.record_in -ne '6') { $failed = 'the Titles track moved although it was switched off' }
  }
  # 2. Undo, then Make room again with the default (Titles on): the titles follow the cut.
  if (-not $failed) {
    $undo = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl')
    $failed = $undo.Errors
    if (-not $failed -and (Get-Timing $run 'V1' 'b.mp4').source_in -ne '0') { $failed = 'undo did not restore the second clip' }
  }
  if (-not $failed) {
    $again = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @(
      'expect @button:make_room'
      'wait 400'
      "shot $work\make_room_ripple.jpg"
      'click @button:make_room'
      'wait 500'
    )
    $failed = $again.Errors
    if (-not $failed) {
      Show $run
      $x = Get-Timing $run 'Titles' 'across'; $l = Get-Timing $run 'Titles' 'later'
      if ($x.record_in -ne '2' -or $x.duration -ne '3/2') { $failed = "the title across the cut was not shortened by half a second ($($x.record_in) +$($x.duration))" }
      elseif ($l.record_in -ne '11/2') { $failed = "the later title did not come up by half a second ($($l.record_in))" }
    }
  }
  # 3. Now it fits: the dissolve is added from the same card.
  if (-not $failed) {
    $dis = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('click @button:add_dissolve')
    $failed = $dis.Errors
    if (-not $failed) {
      $track = Get-Object $run (@(Get-Tracks $run) | Where-Object { $_.name -eq 'V1' }).id
      $t = @($track.transitions.PSObject.Properties | ForEach-Object { $_.Value })
      if ($t.Count -ne 1 -or $t[0].type -ne 'attome.dissolve') { $failed = 'the dissolve was not added after making room' }
    }
  }
  # 4. Two undos restore everything, the titles included.
  if (-not $failed) {
    $undo2 = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl', 'key Z ctrl')
    $failed = $undo2.Errors
    if (-not $failed) {
      Show $run
      $b = Get-Timing $run 'V1' 'b.mp4'; $c = Get-Timing $run 'V1' 'c.mp4'
      $x = Get-Timing $run 'Titles' 'across'; $l = Get-Timing $run 'Titles' 'later'
      if ($b.source_in -ne '0' -or $b.duration -ne '3' -or $c.record_in -ne '6' -or $x.duration -ne '2' -or $l.record_in -ne '6') { $failed = 'undo did not restore the clips and the titles' }
    }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Make room moved the picture track, and the Titles track only while it was switched on; the dissolve was added; undo restored everything (captures in $work)" -ForegroundColor Green
exit 0
