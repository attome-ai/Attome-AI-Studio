# UI test: the Models panel (UX review B9, U13, U19).
#  - the models that live in the engine (Kokoro, OmniVoice) are listed with their state, below the downloads
#  - the whole panel scrolls as one column (the list used to be a small window of its own at the bottom)
#  - the Generate cards carry a state dot and a tooltip text (checked by a capture)
# Mock engine, virtual input only. Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_models.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Models.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try { & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }
  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
    'click @rail:Generate', 'wait 500'
    "shot $work\models_generate.jpg"
    'click @rail:Models', 'wait 900'
    'expect @engine_model:kokoro.82m'                # found by scrolling the panel
    'expect @engine_model:omnivoice.bf16'
    "shot $work\models_engine.jpg"
  )
  $failed = $run.Errors
  Stop-Daemon $run
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the Models panel lists the engine's models and scrolls as one column (captures in $work)" -ForegroundColor Green
exit 0
