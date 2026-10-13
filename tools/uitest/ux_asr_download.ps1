# UI test: "Auto captions" with no speech model on the computer offers the download right there, instead of sending the user to the
# Models panel. The sheet has a download button for each model, with its size; "Not now" closes it. Nothing is downloaded: the download
# button is never clicked. A stand-in speech program (ATTOME_WHISPER_EXE; atm_fake_whisper.exe in the build, or ATTOME_FAKE_WHISPER_EXE)
# is needed, because without a speech program the engine says that first. Without one the test says so and passes.
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_asr_download.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$fake = if ($env:ATTOME_FAKE_WHISPER_EXE) { $env:ATTOME_FAKE_WHISPER_EXE } else { Join-Path $bin 'atm_fake_whisper.exe' }
if (-not (Test-Path $fake)) { Write-Host "PASS: (skipped: needs a stand-in speech program at $fake)" -ForegroundColor Green; exit 0 }
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'AsrDownload.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
$models = Join-Path $work 'models'
New-Item -ItemType Directory -Force $models | Out-Null

$env:ATTOME_MODELS_DIR = $models
$env:ATTOME_WHISPER_EXE = $fake
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    New-Sample "$work\talk.mp4" "--seconds 4 --height 360"
    & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
    [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\talk.mp4"; name = 'talk'; at = '1s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
    $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
    if (-not $r.ok) { throw "setup: $($r.error.message)" }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script @(
    'wait 1200'
    'click @rail:Text', 'wait 400', 'click @tab:Captions', 'wait 400', 'click @captions:auto', 'wait 400'
    'key Right shift', 'key Right shift', 'wait 400'   # 2 s: on the clip
    'click @button:auto_captions', 'wait 800'
    'expect @button:asr_download'
    "shot $work\offer.jpg"
    'click @button:asr_offer_cancel', 'wait 500'
    'absent @button:asr_download'
  )
  $failed = $run.Errors
  Stop-Daemon $run
} finally { Remove-Item Env:\ATTOME_MODELS_DIR, Env:\ATTOME_WHISPER_EXE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Auto captions without a speech model offers the download and 'Not now' closes it (captures in $work)" -ForegroundColor Green
exit 0
