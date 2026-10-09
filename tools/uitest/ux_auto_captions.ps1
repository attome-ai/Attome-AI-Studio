# UI test: auto captions (UX review 3, R1). A clip that speaks has "Auto captions from its speech" in its menu: the editor hears it (asr.transcribe, a
# job), then makes captions with every word timed as it was said. whisper.cpp's sample recording is used (11 s of one sentence). Needs the speech
# program in the build, the small model (models.fetch whisper.small; or ATTOME_MODELS_DIR / ATTOME_WHISPER_MODEL) and the sample
# (.deps/whisper.cpp/samples/jfk.wav); without them the test says so and passes. A second run with no model checks that the editor says what to do.
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_auto_captions.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$sample = Join-Path $root '.deps\whisper.cpp\samples\jfk.wav'
$models = if ($env:ATTOME_MODELS_DIR) { $env:ATTOME_MODELS_DIR } else { Join-Path $env:LOCALAPPDATA 'Attome\models' }
$model = if ($env:ATTOME_WHISPER_MODEL) { $env:ATTOME_WHISPER_MODEL } else { Join-Path $models 'ggml-small.bin' }
if (-not (Test-Path "$bin\attome-whisper.exe") -or -not (Test-Path $sample) -or -not (Test-Path $model)) {
  Write-Host "PASS: (skipped: needs attome-whisper in the build, $sample and the small model at $model)" -ForegroundColor Green
  exit 0
}

$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Speech.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item $sample "$work\jfk.wav" -Force

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" new $proj --rate 30 --canvas 1080x1920 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\jfk.wav"; at = '0s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Captions($run) {
  $t = @(Get-Tracks $run) | Where-Object { $_.name -eq 'Captions' }
  if (-not $t) { return @() }
  @($t.clip_list) | ForEach-Object { Get-Object $run $_.id }
}

$failed = $null
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'rclick @clip:jfk', 'wait 400'
  'expect @menuitem:Auto_captions_from_its_speech'
  "shot $work\menu.jpg"
  'click @menuitem:Auto_captions_from_its_speech', 'wait 600'
  "shot $work\listening.jpg"
  'expect @clip:Caption_1'                 # the captions appear when the listening is done (up to 15 s)
  'wait 800'
  "shot $work\captions.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $caps = @(Captions $run)
    "captions: $($caps.Count): " + (($caps | ForEach-Object { $_.content.text }) -join ' | ')
    $said = ($caps | ForEach-Object { $_.content.text }) -join ' '
    if ($caps.Count -lt 2 -or $caps.Count -gt 6) { $failed = "expected 2 to 6 captions for one sentence, got $($caps.Count)" }
    elseif ($said -notmatch 'country') { $failed = "the captions do not say 'country': $said" }
    elseif ($said -notmatch 'ask') { $failed = "the captions do not say 'ask': $said" }
  }
  if (-not $failed) { # in order, each word inside its caption timed, nothing after the 11 s recording (and its short hold)
    $prev = -1.0
    foreach ($c in (Captions $run)) {
      $in = [double](ConvertFrom-Rational $c.timing.record_in); $len = [double](ConvertFrom-Rational $c.timing.duration)
      if ($in -lt $prev) { $failed = 'the captions are not in order'; break }
      if ($in + $len -gt 12.5) { $failed = "a caption runs to $($in + $len) s, past the 11 s recording"; break }
      $at = @($c.content.words | ForEach-Object { [double](ConvertFrom-Rational $_.at) })
      if ($at[0] -ne 0 -or (($at | Sort-Object) -join ',') -ne ($at -join ',')) { $failed = 'the words of a caption do not start at 0 and go on in order'; break }
      $prev = $in
    }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result | ConvertTo-Json -Compress -Depth 4)" }
  }
} finally { Stop-Daemon $run }

# With no model anywhere, the editor says what to do, and makes nothing.
if (-not $failed) {
  $empty = Join-Path $work 'no-models'
  New-Item -ItemType Directory -Force $empty | Out-Null
  $keep = @{ m = $env:ATTOME_MODELS_DIR; w = $env:ATTOME_WHISPER_MODEL }
  $env:ATTOME_MODELS_DIR = $empty; $env:ATTOME_WHISPER_MODEL = $null
  try {
    $proj2 = Join-Path $work 'NoModel.attome'
    Remove-Item $proj2 -Recurse -Force -ErrorAction SilentlyContinue
    $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup2-$PID"
    & "$bin\attome.exe" new $proj2 --rate 30 | Out-Null
    [IO.File]::WriteAllText("$work\call2.json", (@{ project = $proj2; ops = @(@{ op = 'add_clip'; path = "$work\jfk.wav"; at = '0s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
    $null = & "$bin\attome.exe" --json call timeline.edit "$work\call2.json"
    Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue
    $run2 = Invoke-EditorScript -Project $proj2 -TimeoutSeconds 90 -Script @(
      'wait 1200', 'rclick @clip:jfk', 'wait 400', 'click @menuitem:Auto_captions_from_its_speech', 'wait 900', "shot $work\no_model.jpg")
    $failed = $run2.Errors
    try {
      if (-not $failed -and @(Captions $run2).Count -ne 0) { $failed = 'captions were made without a model' }
    } finally { Stop-Daemon $run2 }
  } finally {
    $env:ATTOME_MODELS_DIR = $keep.m; $env:ATTOME_WHISPER_MODEL = $keep.w
    if (-not $keep.m) { Remove-Item Env:\ATTOME_MODELS_DIR -ErrorAction SilentlyContinue }
    if (-not $keep.w) { Remove-Item Env:\ATTOME_WHISPER_MODEL -ErrorAction SilentlyContinue }
  }
}

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a speaking clip's menu makes word-timed captions from what is said; with no model the editor says what to do and makes nothing (captures in $work)" -ForegroundColor Green
exit 0
