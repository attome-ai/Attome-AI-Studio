# UI test: a model that is not on the computer (UX review 5, V2 and V9). With an empty models folder, a video model's card in the
# Generate panel says "Not installed"; a click still adds its clip, and the toast says the model is missing and where it is
# downloaded; the clip's Generate card (what it needs) comes before its Workflow card. Nothing is downloaded. Virtual input only
# (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_gen_missing.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Missing.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
$models = Join-Path $work 'models' # empty: no model is installed
Remove-Item $models -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $models | Out-Null

$env:ATTOME_MODELS_DIR = $models
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
    $listed = ('{}' | & "$bin\attome.exe" --json call gen.models - | ConvertFrom-Json).result.models
    $model = @($listed | Where-Object { -not $_.installed -and $_.kinds -contains 'text->video' })[0]
    if (-not $model) { $model = @($listed | Where-Object { -not $_.installed })[0] }
    if (-not $model) { throw 'gen.models lists no model that is not installed' }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }
  $name = $model.title.Split(':')[0]
  $toast = ('toast:' + ("Added a clip. $name is not on this computer yet").Substring(0, 24)) -replace ' ', '_'
  "model: $($model.id) ($name)"

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script @(
    'wait 1200'
    'click @rail:Generate', 'wait 600'
    "expect @model:$($model.id)"
    "shot $work\card_not_installed.jpg"
    "click @model:$($model.id)", 'wait 900'
    "expect @$toast"
    'expect @card:Generate', 'expect @card:Workflow'
    'above @card:Generate @card:Workflow'
    "shot $work\clip_added.jpg"
  )
  $failed = $run.Errors
  Stop-Daemon $run
} finally { Remove-Item Env:\ATTOME_MODELS_DIR -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a model that is not installed says so on its card and when its clip is added; the Generate card comes first (captures in $work)" -ForegroundColor Green
exit 0
