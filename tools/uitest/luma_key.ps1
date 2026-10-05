# UI test: a clip gets a Luma key card (the effect is for clips, so the Effects panel has no tile for it): "Add luma
# key" adds it, and the Key level slider sets params.level (0..1). Virtual input only (uitest.psm1). Needs a build
# (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\luma_key.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'LumaKey.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 6 --height 540"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $seq = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].id
  $a = ("$work\a.mp4").Replace('\', '\\')
  @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"a.mp4","timing":{"record_in":"0","duration":"6","source_in":"0"},"media_ref":{"type":"file","path":"$a","duration":"6","width":960,"height":540}}}
]}
"@ | Set-Content "$work\lk.json" -Encoding utf8
  & "$bin\attome.exe" patch $proj "$work\lk.json" | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$run = Invoke-EditorScript -Project $proj -Script @(
  'click @rail:Effects'
  'drag @effect:luma @clip:a.mp4'
  'expect @slider:luma_level'
  'slide @slider:luma_level 1'          # 0 .. 1 on the slider: white
  'wait 800'
  "shot $work\luma_key_card.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $v1 = @(Get-Tracks $run) | Where-Object { $_.name -eq 'V1' }
    $e = @((Get-Object $run $v1.clip_list[0].id).effects.PSObject.Properties | ForEach-Object { $_.Value })[0]
    "key: $($e.effect) level=$($e.params.level) tolerance=$($e.params.tolerance) softness=$($e.params.softness)"
    if ($e.effect -notlike 'attome.luma_key*') { $failed = 'the clip has no luma key' }
    elseif ($e.params.level -lt 0.9) { $failed = 'the Key level slider did not go to white' }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Luma key card on a clip and its Key level slider (captures in $work)" -ForegroundColor Green
exit 0
