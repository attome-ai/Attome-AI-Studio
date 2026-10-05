# UI test: the Effects panel adds a blur as an adjustment layer on an "Effects" track under the titles; the Blur card
# changes its radius. Virtual input only (uitest.psm1). Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\effects.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Effects.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# A video track and a Titles track above it.
$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 4 --height 540"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $seq = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].id
  $a = ("$work\a.mp4").Replace('\', '\\')
  @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"a.mp4","timing":{"record_in":"0","duration":"4","source_in":"0"},"media_ref":{"type":"file","path":"$a","duration":"4","width":960,"height":540}}},
 {"op":"add","path":"$seq/tracks/`$new:t","value":{"kind":"video","name":"Titles"}},
 {"op":"add","path":"`$new:t/clips/`$new:title","value":{"name":"Title","timing":{"record_in":"0","duration":"3","source_in":"0"},"media_ref":{"type":"text"},"content":{"text":"Hello","size":0.12,"color":"#ffffff","bold":true}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
  & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$run = Invoke-EditorScript -Project $proj -Script @(
  'click @rail:Effects'
  'click @effect:blur'               # adds the blur at the playhead and selects it
  'slide @slider:blur_radius 0.5'    # 0 .. 0.1: 0.05
  "shot $work\effects_blur.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $tracks = @(Get-Tracks $run)
    "tracks, bottom first: $(($tracks | ForEach-Object { $_.name }) -join ', ')"
    $fx = $tracks | Where-Object { $_.name -eq 'Effects' }
    if (-not $fx -or $fx.clips -ne 1) { $failed = 'no Effects track with one clip' }
    elseif (($tracks | ForEach-Object { $_.name }) -join ',' -ne 'V1,Effects,Titles') { $failed = 'the Effects track is not between the video and the titles' }
    else {
      $clip = Get-Object $run $fx.clip_list[0].id
      $blur = @($clip.effects.PSObject.Properties | ForEach-Object { $_.Value })[0]
      "adjustment: type=$($clip.media_ref.type) effect=$($blur.effect) radius=$($blur.params.radius)"
      if ($clip.media_ref.type -ne 'adjustment' -or $blur.effect -notlike 'attome.gaussian_blur*') { $failed = 'the clip is not a blur adjustment layer' }
      elseif (-not ($blur.params.radius -gt 0.045 -and $blur.params.radius -lt 0.055)) { $failed = 'the Blur card did not set the radius to about 0.05' }
    }
  }
  if (-not $failed) { # a blur on one clip: the title's own Blur card
    $again = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'click @clip:Title'
      'click @rail:Effects'
      'drag @effect:blur @clip:Title'
      "shot $work\effects_clip_blur.jpg"
    )
    $failed = $again.Errors
    if (-not $failed) {
      $title = Get-Object $run ((@(Get-Tracks $run) | Where-Object { $_.name -eq 'Titles' }).clip_list[0].id)
      $fx = @($title.effects.PSObject.Properties | ForEach-Object { $_.Value })
      "title effects: $($fx | ConvertTo-Json -Compress)"
      if ($fx.Count -ne 1 -or $fx[0].effect -notlike 'attome.gaussian_blur*') { $failed = 'the title clip did not get its own blur' }
    }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a blur adjustment layer from the Effects panel, and a blur on one clip from its card (captures in $work)" -ForegroundColor Green
exit 0
