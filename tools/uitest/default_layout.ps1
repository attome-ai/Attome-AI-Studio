# UI test: with no saved layout (a first start), the bottom panel opens on the Timeline, not History or Profiler.
# Virtual input only (uitest.psm1); test runs always use the default layout. Exit code 0 = pass.
#   .\tools\uitest\default_layout.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Layout.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 4 --height 540"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $seq = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].id
  $a = ("$work\a.mp4").Replace('\', '\\')
  @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"a.mp4","timing":{"record_in":"0","duration":"4","source_in":"0"},"media_ref":{"type":"file","path":"$a","duration":"4","width":960,"height":540}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
  & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

# A clip on the timeline is only drawn (and so found) when the Timeline tab is the one showing. Clicking at once, while
# the panels are still appearing, once let a later panel (the Profiler) take the focus and the front tab.
$run = Invoke-EditorScript -Project $proj -Script @(
  'click @rail:Text'
  'wait 500'
  'expect @clip:a.mp4'
  "shot $work\default_layout.jpg"
)
$failed = $run.Errors
if ($failed) { $failed = "the Timeline is not the tab that shows first ($failed)" }
Stop-Daemon $run

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a first start opens on the Timeline (captures in $work)" -ForegroundColor Green
exit 0
