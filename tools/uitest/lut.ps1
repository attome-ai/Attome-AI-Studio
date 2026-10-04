# UI test: the Effects panel has a LUT tile, and the card of a LUT layer shows its file and a Strength slider that
# changes params.strength. The file dialog is native and cannot be scripted, so the layer is made by a patch and the
# tile is only looked for, not clicked. Virtual input only (uitest.psm1). Needs a build (build\win-msvc-release\bin).
# Exit code 0 = pass.
#   .\tools\uitest\lut.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Lut.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# A 2-point cube that swaps red and blue.
$cube = Join-Path $work 'swap.cube'
$lines = @('LUT_3D_SIZE 2')
foreach ($b in 0, 1) { foreach ($g in 0, 1) { foreach ($r in 0, 1) { $lines += "$b $g $r" } } }
$lines | Set-Content $cube -Encoding ascii

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 6 --height 540"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $seq = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].id
  $a = ("$work\a.mp4").Replace('\', '\\')
  $c = $cube.Replace('\', '\\')
  @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"a.mp4","timing":{"record_in":"0","duration":"6","source_in":"0"},"media_ref":{"type":"file","path":"$a","duration":"6","width":960,"height":540}}},
 {"op":"add","path":"$seq/tracks/`$new:fx","value":{"kind":"video","name":"Effects"}},
 {"op":"add","path":"`$new:fx/clips/`$new:l","value":{"name":"LUT","timing":{"record_in":"0","duration":"6","source_in":"0"},"media_ref":{"type":"adjustment"},"transform":{"opacity":1.0},"effects":{"`$new:e":{"effect":"attome.lut@1.0.0","enabled":true,"params":{"file":"$c","strength":1.0}}}}}
]}
"@ | Set-Content "$work\lut.json" -Encoding utf8
  & "$bin\attome.exe" patch $proj "$work\lut.json" | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$run = Invoke-EditorScript -Project $proj -Script @(
  'click @rail:Effects'
  'expect @effect:lut'                  # the tile
  "shot $work\effects_lut_tile.jpg"
  'click @clip:LUT'
  'expect @button:choose_lut'           # the card: file name and a button to pick another
  'slide @slider:lut_strength 0.5'
  'wait 800'
  "shot $work\lut_card.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $fx = @(Get-Tracks $run) | Where-Object { $_.name -eq 'Effects' }
    $e = @((Get-Object $run $fx.clip_list[0].id).effects.PSObject.Properties | ForEach-Object { $_.Value })[0]
    "lut: $($e.effect) file=$($e.params.file) strength=$($e.params.strength)"
    if ([math]::Abs($e.params.strength - 0.5) -gt 0.05) { $failed = 'the Strength slider did not set 0.5' }
    elseif ($e.params.file -ne $cube) { $failed = 'the LUT file changed' }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the LUT tile, the LUT card and its Strength slider (captures in $work)" -ForegroundColor Green
exit 0
