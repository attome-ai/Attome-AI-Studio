# UI test: select a clip, add a dissolve into the next one from the Inspector, see it on the timeline, remove it.
# Virtual input only (uitest.psm1). Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\dissolve.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP 'attome-uitest'
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Dissolve.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# Two 3-second clips that touch at 3 s, each with media to spare beyond the cut.
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
 {"op":"add","path":"`$new:v1/clips/`$new:b","value":{"name":"b.mp4","timing":{"record_in":"3","duration":"3","source_in":"2"},"media_ref":{"type":"file","path":"$b","duration":"6","width":960,"height":540}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
  & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Get-Transitions($run) {
  $track = Get-Object $run (Get-Tracks $run)[0].id
  if ($track.transitions) { ,@($track.transitions.PSObject.Properties | ForEach-Object { $_.Value }) } else { ,@() }
}

# The first clip is selected; its Inspector offers a dissolve into the next clip.
$run = Invoke-EditorScript -Project $proj -SelectFirstClip -Script @(
  'click @button:add_dissolve'
  "shot $work\dissolve_added.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $t = Get-Transitions $run
    "transitions: $($t.Count)  $($t | ConvertTo-Json -Compress)"
    if ($t.Count -ne 1 -or $t[0].type -ne 'attome.dissolve' -or $t[0].in_offset -ne '1/2' -or $t[0].out_offset -ne '1/2') {
      $failed = 'the Inspector did not add a 1-second dissolve centred on the cut'
    }
  }
  if (-not $failed) { # the same card now removes it
    $again = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('click @button:remove_dissolve')
    $failed = $again.Errors
    if (-not $failed -and (Get-Transitions $run).Count -ne 0) { $failed = 'the Inspector did not remove the dissolve' }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a dissolve was added and removed from the Inspector (captures in $work)" -ForegroundColor Green
exit 0
