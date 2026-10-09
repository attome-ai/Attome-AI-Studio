# UI test: the Models panel lists the catalog with what is on disk: a download that was stopped shows how much it has
# and offers to continue. Nothing is downloaded: the test does not press the button, and its models folder is its own.
# Virtual input only (uitest.psm1). Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\models.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Models.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# A models folder with the first megabytes of one file, as a stopped download leaves them.
$models = Join-Path $work 'models'
Remove-Item $models -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $models 'vae') | Out-Null
[IO.File]::WriteAllBytes((Join-Path $models 'vae\minimax_h3_audio_vae_fp32.safetensors.part'), (New-Object byte[] 3000000))

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try { & "$bin\attome.exe" new $proj --rate 30 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$env:ATTOME_MODELS_DIR = $models
try {
  $run = Invoke-EditorScript -Project $proj -Script @(
    'click @rail:Models'
    'wait 800'                         # the panel asks the daemon twice a second
    'click @button:models_settings', 'wait 300'
    'expect @button:models_folder'
    'expect @button:model_fetch'       # "Continue, ... left"
    'expect @button:model_licence'
    "shot $work\models.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $entry = (Invoke-Attome $run --json models list | ConvertFrom-Json).result.entries[0]
      "entry: $($entry.id) state=$($entry.state) bytes=$($entry.bytes) of $($entry.size), first file $($entry.files[0].state)"
      if ($entry.state -ne 'partial' -or $entry.bytes -ne 3000000) { $failed = 'the stopped download is not listed as partial with its 3 MB' }
      elseif ($entry.files[0].state -ne 'partial') { $failed = 'the first file is not partial' }
      elseif (Test-Path (Join-Path $models 'vae\minimax_h3_audio_vae_fp32.safetensors')) { $failed = 'a file was downloaded' }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MODELS_DIR -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the Models panel lists the catalog and a stopped download (capture in $work)" -ForegroundColor Green
exit 0
