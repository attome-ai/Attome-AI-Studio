# UI test: the export sheet's frame rate and captions file (UX review 5, V10). A 30 fps film with captions: the sheet offers Project /
# 24 / 25 / 30 / 50 / 60 and, as the film has a Captions track, "the captions as an .srt file beside it". 24 and the box: the video is
# made at 24 fps and an .srt with the captions' words is written beside it. Virtual input only (uitest.psm1). Needs a build. Exit code
# 0 = pass.
#   .\tools\uitest\ux_export_fps_srt.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Fps.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$work\out.mp4", "$work\out.srt" -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\v.mp4" "--seconds 3 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(
        @{ op = 'add_clip'; path = "$work\v.mp4"; name = 'v'; at = '0s' },
        @{ op = 'add_captions'; text = 'Hello there. This is a test.'; at = '0s'; duration = '3s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message) $($r.error.hint)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script @(
  'wait 1200'
  'click @button:export', 'wait 800'
  'click @field:export_path', 'key A ctrl', "type $work\out.mp4", 'key Enter', 'wait 300'
  'click @button:export_fps_24', 'wait 300'
  "shot $work\export_sheet.jpg"
  'expect @check:export_srt'
  'click @button:export_start', 'wait 5000'
  "shot $work\export_done.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $probe = (Invoke-Attome $run --json probe "$work\out.mp4" | ConvertFrom-Json).result
    "video: $($probe.rate) fps, $($probe.seconds) s"
    if ($probe.rate -ne '24') { $failed = "the video is $($probe.rate) fps, not 24" }
    elseif (-not (Test-Path "$work\out.srt")) { $failed = 'no out.srt beside the video' }
    else {
      $srt = Get-Content "$work\out.srt" -Raw
      if ($srt -notmatch 'Hello there' -or $srt -notmatch '-->') { $failed = "out.srt does not hold the captions: $srt" }
    }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the export sheet makes the video at 24 fps and writes the captions as an .srt beside it (captures in $work)" -ForegroundColor Green
exit 0
