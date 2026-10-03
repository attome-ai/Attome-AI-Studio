# UI test: the Effects panel adds a colour grade and a vignette as adjustment layers, the Color grade card changes a
# parameter, and the Transition card adds a wipe, a push and a zoom. Virtual input only (uitest.psm1). Needs a build
# (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\grade_wipe.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP 'attome-uitest'
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'GradeWipe.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# Two 3-second clips that touch at 3 s, each with media to spare beyond the cut (for the wipe).
$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" sample "$work\a.mp4" --seconds 6 --height 540 | Out-Null
  & "$bin\attome.exe" sample "$work\b.mp4" --seconds 6 --height 540 | Out-Null
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $seq = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].id
  $a = ("$work\a.mp4").Replace('\', '\\')
  $b = ("$work\b.mp4").Replace('\', '\\')
  @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"a.mp4","timing":{"record_in":"0","duration":"3","source_in":"0"},"media_ref":{"type":"file","path":"$a","duration":"6","width":960,"height":540}}},
 {"op":"add","path":"`$new:v1/clips/`$new:b","value":{"name":"b.mp4","timing":{"record_in":"3","duration":"3","source_in":"2"},"media_ref":{"type":"file","path":"$b","duration":"6","width":960,"height":540}}}
]}
"@ | Set-Content "$work\pg.json" -Encoding utf8
  & "$bin\attome.exe" patch $proj "$work\pg.json" | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Get-FxOf($run, $clip) { @($clip.effects.PSObject.Properties | ForEach-Object { $_.Value }) }

$run = Invoke-EditorScript -Project $proj -Script @(
  'click @rail:Effects'
  'click @effect:grade'                 # a colour grade adjustment layer at the playhead, selected
  'slide @slider:grade_saturation 0'    # 0 .. 3: no colour
  "shot $work\grade_card.jpg"
  'click @effect:vignette'              # a second layer, after the first on the same Effects track
  "shot $work\vignette_card.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $fx = @(Get-Tracks $run) | Where-Object { $_.name -eq 'Effects' }
    if (-not $fx -or $fx.clips -ne 2) { $failed = 'no Effects track with two layers' }
    else {
      $grade = Get-Object $run $fx.clip_list[0].id
      $vig = Get-Object $run $fx.clip_list[1].id
      $g = (Get-FxOf $run $grade)[0]
      $v = (Get-FxOf $run $vig)[0]
      "grade: $($g.effect) saturation=$($g.params.saturation)   vignette: $($v.effect) strength=$($v.params.strength)"
      if ($g.effect -notlike 'attome.color_grade*') { $failed = 'the first layer is not a colour grade' }
      elseif ($g.params.saturation -gt 0.05) { $failed = 'the Color grade card did not set saturation to 0' }
      elseif ($v.effect -notlike 'attome.vignette*') { $failed = 'the second layer is not a vignette' }
    }
  }
  if (-not $failed) { # a wipe from the bottom, added from the first clip's Transition card
    $again = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @(
      'click @button:wipe_down'
      'click @button:add_wipe'
      "shot $work\wipe_added.jpg"
    )
    $failed = $again.Errors
    if (-not $failed) {
      $track = Get-Object $run (@(Get-Tracks $run) | Where-Object { $_.name -eq 'V1' }).id
      $t = @($track.transitions.PSObject.Properties | ForEach-Object { $_.Value })
      "transition: $($t | ConvertTo-Json -Compress)"
      if ($t.Count -ne 1 -or $t[0].type -ne 'attome.wipe' -or $t[0].params.direction -ne 'down') { $failed = 'the Transition card did not add a wipe from the bottom' }
    }
  }
  if (-not $failed) { # the same card removes it
    $gone = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('click @button:remove_wipe')
    $failed = $gone.Errors
    if (-not $failed) {
      $track = Get-Object $run (@(Get-Tracks $run) | Where-Object { $_.name -eq 'V1' }).id
      if ($track.transitions -and @($track.transitions.PSObject.Properties).Count -ne 0) { $failed = 'the Transition card did not remove the wipe' }
    }
  }
  if (-not $failed) { # a push from the top, the same card
    $push = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @(
      'click @button:wipe_up'
      'click @button:add_push'
      "shot $work\push_added.jpg"
    )
    $failed = $push.Errors
    if (-not $failed) {
      $track = Get-Object $run (@(Get-Tracks $run) | Where-Object { $_.name -eq 'V1' }).id
      $t = @($track.transitions.PSObject.Properties | ForEach-Object { $_.Value })
      "transition: $($t | ConvertTo-Json -Compress)"
      if ($t.Count -ne 1 -or $t[0].type -ne 'attome.push' -or $t[0].params.direction -ne 'up') { $failed = 'the Transition card did not add a push from the top' }
    }
  }
  if (-not $failed) {
    $gone = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('click @button:remove_push')
    $failed = $gone.Errors
    if (-not $failed) {
      $track = Get-Object $run (@(Get-Tracks $run) | Where-Object { $_.name -eq 'V1' }).id
      if ($track.transitions -and @($track.transitions.PSObject.Properties).Count -ne 0) { $failed = 'the Transition card did not remove the push' }
    }
  }
  if (-not $failed) { # a zoom with an amount from the slider, the same card
    $zoom = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @(
      'slide @slider:zoom_amount 0.5'      # 0.05 .. 2: about 1.03
      'click @button:add_zoom'
      "shot $work\zoom_added.jpg"
    )
    $failed = $zoom.Errors
    if (-not $failed) {
      $track = Get-Object $run (@(Get-Tracks $run) | Where-Object { $_.name -eq 'V1' }).id
      $t = @($track.transitions.PSObject.Properties | ForEach-Object { $_.Value })
      "transition: $($t | ConvertTo-Json -Compress)"
      if ($t.Count -ne 1 -or $t[0].type -ne 'attome.zoom' -or [math]::Abs($t[0].params.amount - 1.03) -gt 0.08) { $failed = 'the Transition card did not add a zoom of about 1.03' }
    }
  }
  if (-not $failed) {
    $gone = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('click @button:remove_zoom')
    $failed = $gone.Errors
    if (-not $failed) {
      $track = Get-Object $run (@(Get-Tracks $run) | Where-Object { $_.name -eq 'V1' }).id
      if ($track.transitions -and @($track.transitions.PSObject.Properties).Count -ne 0) { $failed = 'the Transition card did not remove the zoom' }
    }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: colour grade and vignette layers, a grade parameter, and a wipe, a push and a zoom added and removed (captures in $work)" -ForegroundColor Green
exit 0
