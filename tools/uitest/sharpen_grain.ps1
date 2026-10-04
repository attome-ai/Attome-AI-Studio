# UI test: the Effects panel adds Sharpen and Film grain as adjustment layers, and their cards' sliders change the
# parameters (ranges from the shared effect table). Virtual input only (uitest.psm1). Needs a build
# (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\sharpen_grain.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'SharpenGrain.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 8 --height 540"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $seq = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].id
  $a = ("$work\a.mp4").Replace('\', '\\')
  @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"a.mp4","timing":{"record_in":"0","duration":"8","source_in":"0"},"media_ref":{"type":"file","path":"$a","duration":"8","width":960,"height":540}}}
]}
"@ | Set-Content "$work\psg.json" -Encoding utf8
  & "$bin\attome.exe" patch $proj "$work\psg.json" | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Get-FxOf($run, $clip) { @($clip.effects.PSObject.Properties | ForEach-Object { $_.Value })[0] }

$run = Invoke-EditorScript -Project $proj -Script @(
  'click @rail:Effects'
  "shot $work\effects_five.jpg"
  'click @effect:sharpen'              # a Sharpen layer at the playhead, selected
  'slide @slider:sharpen_amount 1'     # 0 .. 3 on the slider: 3
  'click @effect:grain'                # a Film grain layer after it
  'slide @slider:grain_size 0.5'       # 1 .. 6 on the slider: 3.5
  'wait 500'
  "shot $work\grain_card.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $fx = @(Get-Tracks $run) | Where-Object { $_.name -eq 'Effects' }
    if (-not $fx -or $fx.clips -ne 2) { $failed = 'no Effects track with two layers' }
    else {
      $sharp = Get-FxOf $run (Get-Object $run $fx.clip_list[0].id)
      $grain = Get-FxOf $run (Get-Object $run $fx.clip_list[1].id)
      "sharpen: $($sharp.effect) amount=$($sharp.params.amount) radius=$($sharp.params.radius)   grain: $($grain.effect) strength=$($grain.params.strength) size=$($grain.params.size)"
      if ($sharp.effect -notlike 'attome.sharpen*') { $failed = 'the first layer is not a Sharpen layer' }
      elseif ([math]::Abs($sharp.params.amount - 3.0) -gt 0.05) { $failed = 'the Sharpen card did not set the amount to the top of its slider (3)' }
      elseif ($grain.effect -notlike 'attome.film_grain*') { $failed = 'the second layer is not a Film grain layer' }
      elseif ([math]::Abs($grain.params.size - 3.5) -gt 0.1) { $failed = 'the Film grain card did not set the size to the middle of its slider (3.5)' }
    }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Sharpen and Film grain layers from the Effects panel, and their sliders (captures in $work)" -ForegroundColor Green
exit 0
