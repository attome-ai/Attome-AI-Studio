# UI test: J / K / L, the shuttle (UX review 3, R8). L plays, J plays backwards; each again doubles the speed (shown next to the
# transport as "2x", "4x back"), the other key turns round at 1x, K stops. Played backwards the film stops at its start. Virtual
# input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_shuttle.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Shuttle.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_text'; text = 'Shuttle'; name = 'Shuttle'; at = '0s'; duration = '4s' }) } | ConvertTo-Json -Depth 5 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script @(
  'wait 1200'
  'key Home', 'wait 300'
  'key L', 'wait 300', 'expect @transport:playing', 'absent @transport:speed:1'   # plain playing: no speed shown
  'key L', 'wait 200', 'expect @transport:speed:2'                               # faster
  'key L', 'wait 200', 'expect @transport:speed:4'
  "shot $work\forward_4x.jpg"
  'key J', 'wait 300', 'expect @transport:speed:-1'                              # turned round: backwards at 1x
  "shot $work\back_1x.jpg"
  'key J', 'wait 300', 'expect @transport:speed:-2'
  'key L', 'wait 300', 'expect @transport:playing', 'absent @transport:speed:-2' # forwards at 1x again, with its sound
  'key K', 'wait 400', 'absent @transport:playing'
  'key End', 'wait 200'
  'key J', 'key J', 'key J', 'wait 200', 'expect @transport:speed:-4'             # 4 s at 4x back: a second to the start
  'wait 2500', 'absent @transport:playing'                                        # stopped there
  "shot $work\back_at_start.jpg"
)
$failed = $run.Errors
Stop-Daemon $run

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: J plays backwards, L forwards, again goes faster, K stops, and backwards stops at the start (captures in $work)" -ForegroundColor Green
exit 0
