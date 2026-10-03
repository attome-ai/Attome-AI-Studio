# UI test: a clip gets a Chroma key card (the effect is for clips, so the Effects panel has no tile for it): "Add chroma
# key" adds it, and the Key colour slider sets params.hue (0..360). Virtual input only (uitest.psm1). Needs a build
# (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\chroma_key.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP 'attome-uitest'
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'ChromaKey.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" sample "$work\a.mp4" --seconds 6 --height 540 | Out-Null
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $seq = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].id
  $a = ("$work\a.mp4").Replace('\', '\\')
  @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"a.mp4","timing":{"record_in":"0","duration":"6","source_in":"0"},"media_ref":{"type":"file","path":"$a","duration":"6","width":960,"height":540}}}
]}
"@ | Set-Content "$work\ck.json" -Encoding utf8
  & "$bin\attome.exe" patch $proj "$work\ck.json" | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$run = Invoke-EditorScript -Project $proj -Script @(
  'click @clip:a.mp4'
  'click @button:add_key'
  'expect @slider:key_hue'
  'slide @slider:key_hue 0.6667'        # 0 .. 360 on the slider: 240, blue
  'wait 800'
  "shot $work\chroma_key_card.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $v1 = @(Get-Tracks $run) | Where-Object { $_.name -eq 'V1' }
    $e = @((Get-Object $run $v1.clip_list[0].id).effects.PSObject.Properties | ForEach-Object { $_.Value })[0]
    "key: $($e.effect) hue=$($e.params.hue) similarity=$($e.params.similarity) smoothness=$($e.params.smoothness)"
    if ($e.effect -notlike 'attome.chroma_key*') { $failed = 'the clip has no chroma key' }
    elseif ([math]::Abs($e.params.hue - 240) -gt 12) { $failed = 'the Key colour slider did not set about 240' }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Chroma key card on a clip and its Key colour slider (captures in $work)" -ForegroundColor Green
exit 0
