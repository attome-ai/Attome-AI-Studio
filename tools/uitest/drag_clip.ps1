# UI regression test: dragging the picture in the Monitor moves the selected clip and saves one "Move clip" edit.
# Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
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
$endpoint = "\\.\pipe\attome-uitest-setup-$PID"
$env:ATTOME_ENDPOINT = $endpoint
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

$ed = Start-Editor -Project $proj -SelectFirstClip
$failed = $null
try {
  Show-Window $ed
  $cid = (Invoke-Attome $ed --json inspect $proj --level tracks | ConvertFrom-Json).result.data.sequences[0].tracks[0].clip_list[0].id
  $pos = { (Invoke-Attome $ed --json get $proj $cid | ConvertFrom-Json).result.object.transform.position }
  $before = & $pos
  Save-Shot $ed (Join-Path $work 'drag_before.png') | Out-Null
  # The clip is half size and centred in the Monitor; the Monitor picture is centred in the window.
  Move-Drag $ed 853 320 703 425
  Save-Shot $ed (Join-Path $work 'drag_after.png') | Out-Null
  $after = & $pos
  "position before: $($before -join ', ')   after: $($after -join ', ')"
  if ([math]::Abs($before[0] - 0.5) -gt 0.001) { $failed = "unexpected start position" }
  elseif (-not ($after[0] -lt 0.30 -and $after[0] -gt 0.20)) { $failed = "x did not move to about 0.27" }
  elseif (-not ($after[1] -gt 0.70 -and $after[1] -lt 0.85)) { $failed = "y did not move to about 0.79" }
  $edits = (Invoke-Attome $ed --json history $proj | ConvertFrom-Json).result.changesets | Where-Object { $_.label -eq 'Move clip' }
  if (-not $failed -and @($edits).Count -ne 1) { $failed = "expected exactly one 'Move clip' edit, found $(@($edits).Count)" }
} finally { Stop-Editor $ed }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: dragging the picture moved the clip (captures in $work)" -ForegroundColor Green
