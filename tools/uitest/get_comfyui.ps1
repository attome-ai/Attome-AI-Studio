# UI test: "Get ComfyUI" in the Models panel. With no ComfyUI of Attome's on the computer it offers "Install ComfyUI"; pressing it shows
# what would be downloaded, from whom and under which licence, before anything starts; "Not now" goes back. Nothing is downloaded: the
# "Download and install" button is never clicked. The settings and the models folder are the test's own.
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\get_comfyui.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'GetComfy.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
$settings = Join-Path $work 'settings.json'
Remove-Item $settings -Force -ErrorAction SilentlyContinue

$env:ATTOME_SETTINGS = $settings
$env:ATTOME_MODELS_DIR = Join-Path $work 'models'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try { & "$bin\attome.exe" new $proj --rate 30 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -Script @(
    'click @rail:Models'
    'wait 500'
    'click @button:models_settings', 'wait 600'
    'expect @button:comfy_get_install'
    "shot $work\get_offer.jpg"
    'click @button:comfy_get_install', 'wait 400'
    'expect @button:comfy_get_go'
    'expect @button:comfy_get_cancel'
    "shot $work\get_confirm.jpg"
    'click @button:comfy_get_cancel', 'wait 400'
    'expect @button:comfy_get_install'
    'absent @button:comfy_get_go'
  )
  $failed = $run.Errors
  Stop-Daemon $run
} finally { Remove-Item Env:\ATTOME_SETTINGS, Env:\ATTOME_MODELS_DIR -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Get ComfyUI offers the install, shows what it fetches and the licences first, and 'Not now' goes back (captures in $work)" -ForegroundColor Green
exit 0
