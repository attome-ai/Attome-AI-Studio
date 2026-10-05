# UI test: a generative clip whose model is "not installed" because its files are in another folder. The card offers
# "I already have it..."; pointing it at the folder that holds the files makes the clip ready with nothing downloaded,
# and the Models panel lists the folder. The folder picker is answered by ATTOME_EDITOR_PICK_FOLDER (a script cannot
# drive the native dialog). The files are empty ones of the right size; the models folder and the settings file are the
# test's own. Virtual input only (uitest.psm1). Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\generate_locate.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Locate.attome'
$settings = Join-Path $work 'settings.json'
$models = Join-Path $work 'models'          # Attome's own models folder: empty
$comfy = Join-Path $work 'ComfyUI_portable' # the folder a person would pick: ComfyUI's own, not its models folder

# The six files of the catalog's model, where a ComfyUI keeps them. Only the size is looked at.
$files = [ordered]@{
  'vae\minimax_h3_audio_vae_fp32.safetensors'                           = 605254808
  'loras\minimax_h3_fl2v_turbo_8step_v1.0_comfyui_bf16.safetensors'     = 1956193000
  'vae\minimax_h3_video_vae_int8_convrot.safetensors'                   = 2811065184
  'vae\minimax_h3_video_vae_fp16.safetensors'                           = 5207808496
  'text_encoders\qwen3vl_32b_minimax_h3_nvfp4_awq.safetensors'          = 15687142551
  'diffusion_models\minimax_h3_fl2va_pruned_int8_convrot.safetensors'   = 20970379616
}
foreach ($name in $files.Keys) {
  $path = Join-Path $comfy "ComfyUI\models\$name"
  New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
  $stream = [IO.File]::Create($path)
  try { $stream.SetLength([int64]$files[$name]) } finally { $stream.Close() } # no bytes are written: the length is set
}

# A ComfyUI address makes an engine that runs the model, so "ready" depends on the files alone; nothing is sent to it.
'{"comfyui": "http://127.0.0.1:9"}' | Set-Content $settings -Encoding ascii
$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
$env:ATTOME_SETTINGS = $settings
$env:ATTOME_MODELS_DIR = $models
$env:ATTOME_EDITOR_PICK_FOLDER = $comfy
try {
  try {
    & "$bin\attome.exe" new $proj --rate 24 --canvas 1280x704 | Out-Null
    $info = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data
    $prj = $info.id; $seq = $info.sequences[0].id
    @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"Shot","timing":{"record_in":"0","duration":"5","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":$(Get-VideoInstance -Model 'minimax-h3.fl2va.turbo8-int8' -Settings '{"steps":8}' -Inputs '{"seconds":5,"width":1280,"height":704}' -Exposed @('prompt')),"inputs":{"prompt":"A robot walks"}}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
    & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'the setup patch was refused' }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -Script @(
    'click @clip:Shot'
    'wait 800'
    'expect @button:gen_download'
    'expect @button:gen_locate'
    "shot $work\locate_before.jpg"          # not installed: the download, the other way, and where it looked
    'click @button:gen_locate'
    'wait 1500'
    'expect @field:prompt'                  # the card is the ready one now
    "shot $work\locate_after.jpg"
    'click @rail:Models'
    'wait 800'
    'expect @button:models_forget'          # the folder is listed, and can be dropped
    "shot $work\locate_models.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      # That the clip became ready is what `expect @field:prompt` saw: only the ready card has the prompt.
      $saved = Get-Content $settings -Raw | ConvertFrom-Json
      "remembered: $($saved.model_folders -join ', ')"
      if (@($saved.model_folders).Count -ne 1 -or $saved.model_folders[0] -ne (Join-Path $comfy 'ComfyUI\models')) { $failed = 'the folder was not remembered in the settings file' }
      elseif (Test-Path (Join-Path $models 'vae')) { $failed = 'something was downloaded' }
    }
  } finally { Stop-Daemon $run }
} finally {
  Remove-Item Env:\ATTOME_SETTINGS, Env:\ATTOME_MODELS_DIR, Env:\ATTOME_EDITOR_PICK_FOLDER -ErrorAction SilentlyContinue
  Remove-Item $comfy -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: files the user already has make the clip ready with no download (captures in $work)" -ForegroundColor Green
exit 0
