# UI regression test: dragging the picture in the Monitor moves the selected clip and saves one "Move clip" edit.
# Virtual input only (uitest.psm1). Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\drag_clip.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP 'attome-uitest'
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Drag.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# A scratch project made with a private daemon, so nothing of the user's is touched.
$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" sample "$work\a.mp4" --seconds 4 --height 540 | Out-Null
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $seq = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].id
  $media = ("$work\a.mp4").Replace('\', '\\')
  @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"a.mp4","timing":{"record_in":"0","duration":"4","source_in":"0"},"media_ref":{"type":"file","path":"$media","duration":"4","width":960,"height":540},"transform":{"opacity":1.0,"position":[0.5,0.5],"scale":[0.5,0.5]}}}
]}
"@ | Set-Content "$work\p.json" -Encoding utf8
  & "$bin\attome.exe" patch $proj "$work\p.json" | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

# The half-size clip sits in the middle of the Monitor picture: drag it a quarter of the picture left, 30 % down.
$run = Invoke-EditorScript -Project $proj -SelectFirstClip -Script @(
  'drag @monitor -25% 30%'
  "shot $work\drag_after.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $cid = (Get-Tracks $run)[0].clip_list[0].id
    $after = (Get-Object $run $cid).transform.position
    "position after: $($after -join ', ')"
    if (-not ($after[0] -lt 0.30 -and $after[0] -gt 0.20)) { $failed = "x did not move to about 0.25" }
    elseif (-not ($after[1] -gt 0.72 -and $after[1] -lt 0.88)) { $failed = "y did not move to about 0.80" }
    $edits = (Invoke-Attome $run --json history $proj | ConvertFrom-Json).result.changesets | Where-Object { $_.label -eq 'Move clip' }
    if (-not $failed -and @($edits).Count -ne 1) { $failed = "expected exactly one 'Move clip' edit, found $(@($edits).Count)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: dragging the picture moved the clip (captures in $work)" -ForegroundColor Green
exit 0
