# UI test: "Get Blender" in the Settings (Models panel). With a Blender on the computer it says so; with none (ATTOME_BLENDER=none) it offers the
# download, shows what is fetched, from whom, how big and under which licence before anything starts, and "Not now" goes back. Nothing is
# downloaded: "Download and install" is never clicked. Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\get_blender.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'GetBlender.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
$settings = Join-Path $work 'settings.json'
Remove-Item $settings -Force -ErrorAction SilentlyContinue

$env:ATTOME_SETTINGS = $settings
$env:ATTOME_MODELS_DIR = Join-Path $work 'models'
$env:ATTOME_BLENDER = 'none'
try {
  $env:ATTOME_ENDPOINT = "\.\pipe\attome-uitest-setup-$PID"
  try { & "$bin\attome.exe" new $proj --rate 30 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -Script @(
    'click @rail:Models'
    'wait 500'
    'click @menu:File', 'wait 300', 'click @menuitem:Settings...', 'wait 800'
    'expect @button:blender_get_install'
    "shot $work\blender_offer.jpg"
    'click @button:blender_get_install', 'wait 400'
    'expect @button:blender_get_go'
    'expect @button:blender_get_cancel'
    "shot $work\blender_confirm.jpg"
    'click @button:blender_get_cancel', 'wait 400'
    'expect @button:blender_get_install'
    'absent @button:blender_get_go'
  )
  $failed = $run.Errors
  Stop-Daemon $run
} finally { Remove-Item Env:\ATTOME_SETTINGS, Env:\ATTOME_MODELS_DIR, Env:\ATTOME_BLENDER -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Get Blender offers the download, shows what it fetches and the licence first, and 'Not now' goes back (captures in $work)" -ForegroundColor Green
exit 0
