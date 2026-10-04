# UI test: a generative clip whose model is not installed is drawn red on the timeline, and its Inspector card names
# the model and offers the download. Nothing is downloaded: the test does not press the button, and its models folder is
# its own (and empty). Virtual input only (uitest.psm1). Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\generate_missing.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Missing.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
$models = Join-Path $work 'models'
Remove-Item $models -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $models | Out-Null

# A "Shot" workflow on the catalog's model, and one clip that uses it.
$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
$env:ATTOME_MODELS_DIR = $models
try {
  try {
    & "$bin\attome.exe" new $proj --rate 24 --canvas 1280x704 | Out-Null
    $info = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data
    $prj = $info.id; $seq = $info.sequences[0].id
    @"
{"ops":[
 {"op":"add","path":"$prj/workflows/`$new:shot","value":{"name":"Shot",
   "nodes":{"`$new:gen":{"kind":"attome.generate_video","model":"minimax-h3.fl2va.turbo8-int8","settings":{"steps":8},"inputs":{"seconds":5,"width":1280,"height":704}}},
   "exposed":{"inputs":{"prompt":["`$new:gen","prompt"]},"outputs":{"video":["`$new:gen","video"]}}}},
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"Shot","timing":{"record_in":"0","duration":"5","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":"`$new:shot","inputs":{"prompt":"A robot walks"}}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
    & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'the setup patch was refused' }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -Script @(
    "shot $work\missing_timeline.jpg"       # the clip, red, before it is selected
    'click @clip:Shot'
    'wait 800'
    'expect @button:gen_download'
    "shot $work\missing_card.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $check = (Invoke-Attome $run --json validate $proj | ConvertFrom-Json).result
      "validate: ok=$($check.ok) warnings=$(@($check.warnings).Count) first=$(@($check.warnings)[0].rule)"
      if (-not $check.ok) { $failed = 'the project with a missing model is not valid' }
      elseif (@($check.warnings)[0].rule -ne 'G_MODEL_MISSING') { $failed = 'no G_MODEL_MISSING warning' }
      elseif (@(Get-ChildItem $models -Recurse -File).Count -ne 0) { $failed = 'something was downloaded' }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MODELS_DIR -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a clip whose model is missing is red and offers the download (captures in $work)" -ForegroundColor Green
exit 0
