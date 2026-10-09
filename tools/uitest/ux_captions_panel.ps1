# UI test: Auto captions from the Text panel (UX review 5, V5). The Captions tab has an Auto captions part: with nothing to hear it
# says how to choose a clip; with the playhead on a clip that has sound it names that clip, and its button asks the engine to listen
# (here with an empty models folder: the editor answers that the speech model, or in a build without it the speech program, is
# missing; nothing is downloaded or transcribed). Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_captions_panel.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Captions.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
$models = Join-Path $work 'models'
New-Item -ItemType Directory -Force $models | Out-Null

$env:ATTOME_MODELS_DIR = $models
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    New-Sample "$work\talk.mp4" "--seconds 4 --height 360"
    & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
    [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\talk.mp4"; name = 'talk'; at = '1s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
    $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
    if (-not $r.ok) { throw "setup: $($r.error.message)" }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }
  # What the engine answers: no speech program in this build, or no model in the empty folder.
  $answer = if (Test-Path (Join-Path $bin 'attome-whisper.exe')) { 'No speech model is on this computer yet' } else { 'The speech program is not part of this build' }
  $toast = ('toast:' + $answer.Substring(0, 24)) -replace ' ', '_'

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script @(
    'wait 1200'
    'click @rail:Text', 'wait 400', 'click @tab:Captions', 'wait 400'
    'key Home', 'wait 300'                       # 0 s: before the clip, nothing to hear
    'expect @button:auto_captions'
    "shot $work\nothing_to_hear.jpg"
    'click @button:auto_captions', 'wait 600'    # off: no toast
    "absent @$toast"
    'key Right shift', 'key Right shift', 'wait 400'   # 2 s: on the clip
    "shot $work\clip_to_hear.jpg"
    'click @button:auto_captions', 'wait 1500'
    "expect @$toast"
    "shot $work\answer.jpg"
  )
  $failed = $run.Errors
  Stop-Daemon $run
} finally { Remove-Item Env:\ATTOME_MODELS_DIR -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the Text panel's Auto captions names the clip it hears and asks the engine (captures in $work)" -ForegroundColor Green
exit 0
