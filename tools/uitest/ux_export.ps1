# UI test: the Export sheet (UX review U1, B12). Export opens a sheet with where to save, the size, the quality and the sound; the
# render uses what was chosen (a 720p small file with no sound here), and the same window shows what it came to.
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_export.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Export.attome'
$out = Join-Path $work 'out.mp4'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $out -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 2 --height 720"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $call = @{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\a.mp4"; at = '0s' }) }
  [IO.File]::WriteAllText("$work\call.json", ($call | ConvertTo-Json -Depth 5 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$failed = $null
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'click @button:export', 'wait 500'
  'expect @field:export_path'
  "shot $work\export_sheet.jpg"
  'click @field:export_path'
  'key A ctrl'
  "type $out"
  'click @button:export_res_720p'
  'click @button:export_quality_Small'
  'click @check:export_sound', 'wait 300'          # no sound
  "shot $work\export_chosen.jpg"
  'click @button:export_start'
  'wait 2500'
  'expect @button:export_play'                     # it is done
  "shot $work\export_done.jpg"
  'click @button:export_close', 'wait 300'
)
$failed = $run.Errors
Stop-Daemon $run
if (-not $failed) {
  if (-not (Test-Path $out)) { $failed = 'no file was written' }
  else {
    $info = (& "$bin\attome.exe" --json probe $out | ConvertFrom-Json).result
    "file: $($info.width)x$($info.height), $([math]::Round($info.seconds,1)) s, audio: $($info.has_audio), $([math]::Round((Get-Item $out).Length / 1KB)) KB"
    if ($info.height -ne 720 -and $info.width -ne 720) { $failed = "the size is $($info.width)x$($info.height), not 720p" }
    elseif ($info.has_audio) { $failed = 'the sound was switched off but the file has sound' }
    elseif ((Get-Item $out).Length -gt 600KB) { $failed = "the Small quality made a $([math]::Round((Get-Item $out).Length / 1KB)) KB file for 2 s: too big" }
  }
}

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the Export sheet takes a path, a size, a quality and the sound, and the file is what was chosen (captures in $work)" -ForegroundColor Green
exit 0
