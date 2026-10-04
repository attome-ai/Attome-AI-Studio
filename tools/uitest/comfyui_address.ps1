# UI test: the ComfyUI address is typed into its field in the Models panel; "Use and test" stores it and says whether
# ComfyUI answers. Nothing listens on the address used here, so the answer is "No answer" with a hint. The settings go
# to a file of the test's own. Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\comfyui_address.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Address.attome'
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
    "shot $work\address_empty.jpg"
    'click @field:comfyui'
    'type 127.0.0.1:59999/'             # no scheme and a trailing slash: stored as http://127.0.0.1:59999
    'wait 200'
    'click @button:comfy_test'
    'wait 4000'                         # the connection is refused within a few seconds
    "shot $work\address_no_answer.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $stored = if (Test-Path $settings) { (Get-Content $settings -Raw | ConvertFrom-Json).comfyui } else { '' }
      $engines = (Invoke-Attome $run --json gen engines | ConvertFrom-Json).result
      "stored: '$stored'; engines: $($engines.engines | ForEach-Object { "$($_.name) at $($_.address) reachable=$($_.reachable)" })"
      if ($stored -ne 'http://127.0.0.1:59999') { $failed = "the typed address was stored as '$stored'" }
      elseif (@($engines.engines).Count -ne 1 -or $engines.engines[0].reachable) { $failed = 'the engine list does not show one ComfyUI that does not answer' }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_SETTINGS, Env:\ATTOME_MODELS_DIR -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a typed ComfyUI address is stored and tested, and an address nothing answers on says so (captures in $work)" -ForegroundColor Green
exit 0
