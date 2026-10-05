# UI test: the Text, Effects and Generate panels show their cards as a grid of tiles (captures to look at).
# Mock engine, virtual input only.   .\tools\uitest\gallery.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Gallery.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try { & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }
  $run = Invoke-EditorScript -Project $proj -Script @(
    'click @rail:Text'
    'expect @style:Title'
    "shot $work\gallery_text.jpg"
    'click @rail:Effects'
    'expect @effect:blur'
    "shot $work\gallery_effects.jpg"
    'click @rail:Generate'
    'expect @model:attome-mock'
    "shot $work\gallery_generate.jpg"
  )
  try { Stop-Daemon $run } catch {}
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }
if ($run.Errors) { Write-Host "FAIL: $($run.Errors)" -ForegroundColor Red; exit 1 }
Write-Host "PASS: captures in $work" -ForegroundColor Green
