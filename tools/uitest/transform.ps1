# UI test: the Transform card turns a clip a quarter (+90°) and crops it, the Monitor selects the clip where its turned
# picture now lies, and "Reset transform" puts everything back. Virtual input only (uitest.psm1). Exit code 0 = pass.
#   .\tools\uitest\transform.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Transform.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# A 960 x 540 clip on a 1920 x 1080 canvas at scale 0.4: upright it covers y 324..756; turned a quarter, y 156..924.
$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 4 --height 540"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $seq = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].id
  $a = ("$work\a.mp4").Replace('\', '\\')
  @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"a.mp4","timing":{"record_in":"0","duration":"4","source_in":"0"},"media_ref":{"type":"file","path":"$a","duration":"4","width":960,"height":540},"transform":{"opacity":1,"scale":[0.4,0.4]}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
  & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Get-Clip($run) { Get-Object $run (Get-Tracks $run)[0].clip_list[0].id }

# Turn, crop the right side (the bottom once turned), then click off the clip and back on it at y ~ 200: only the
# turned picture reaches up there, so the Inspector shows the clip again only if the Monitor follows the turn.
$run = Invoke-EditorScript -Project $proj -SelectFirstClip -Script @(
  'click @button:rotate_right'
  'wait 400'
  'drag @crop:right 40 0'
  'wait 400'
  'click @monitor@0.05,0.05'
  'wait 300'
  'click @monitor@0.5,0.185'
  'wait 300'
  'expect @button:reset_transform'
  "shot $work\transform_turned.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $tr = (Get-Clip $run).transform
    "after turning and cropping: $($tr | ConvertTo-Json -Compress -Depth 5)"
    $right = if ($tr.crop) { [double]$tr.crop.right } else { 0 }
    if ([double]$tr.rotation -ne 90) { $failed = "the +90 button should turn the clip to 90 degrees (got $($tr.rotation))" }
    elseif ($right -lt 0.04 -or $right -gt 0.2) { $failed = "dragging the R crop field should cut about 10% (got $right)" }
  }
  if (-not $failed) {
    $reset = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @(
      'click @button:reset_transform'
      'wait 400'
      "shot $work\transform_reset.jpg"
    )
    $failed = $reset.Errors
    if (-not $failed) {
      $tr = (Get-Clip $run).transform
      "after reset: $($tr | ConvertTo-Json -Compress -Depth 5)"
      $cut = if ($tr.crop) { @($tr.crop.PSObject.Properties | Where-Object { [double]$_.Value -ne 0 }).Count } else { 0 }
      if ([double]$tr.rotation -ne 0 -or $cut -ne 0 -or [double]$tr.scale[0] -ne 1) {
        $failed = 'Reset transform should clear the turn, the crop and the scale'
      }
    }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a quarter turn and a crop from the Transform card, selected where it now lies, then reset (captures in $work)" -ForegroundColor Green
exit 0
