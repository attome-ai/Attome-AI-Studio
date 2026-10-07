# UI test: the first clip of a project. Files imported earlier (and not on the timeline) are listed in the Media panel when the project is
# opened; the first clip dropped on the empty timeline starts at 0 wherever it is dropped; a canvas shape that was chosen (9:16) stays.
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_first_clip.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'First.attome'
Remove-Item -LiteralPath $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\v.mp4" "--seconds 4 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 --canvas 1080x1920 | Out-Null
  $pj = @{ project = $proj; paths = @("$work\v.mp4") } | ConvertTo-Json -Compress
  [IO.File]::WriteAllText("$work\call.json", $pj, (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call media.import "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$failed = $null
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
  'wait 1500'
  'expect @media:v.mp4'
  'drag @media:v.mp4 @lane:Video@0.6,0.5 hold', 'wait 200', 'release', 'wait 900'
  "shot $work\first.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $t = @(Get-Tracks $run | Where-Object { $_.kind -eq 'video' })
    $c = Get-Object $run $t[0].clip_list[0].id
    $start = [double](ConvertFrom-Rational $c.timing.start)
    "first clip starts at $start s"
    if ($start -ne 0) { $failed = "the first clip starts at $start s, not 0" }
  }
  if (-not $failed) {
    $info = (Invoke-Attome $run --json inspect $proj | ConvertFrom-Json).result
    $text = $info | ConvertTo-Json -Compress -Depth 8
    if ($text -notmatch '1080' -or $text -notmatch '1920') { $failed = "the 9:16 canvas did not stay: $text" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: imported media is listed on opening, the first clip starts at 0, the chosen 9:16 canvas stays (captures in $work)" -ForegroundColor Green
exit 0
