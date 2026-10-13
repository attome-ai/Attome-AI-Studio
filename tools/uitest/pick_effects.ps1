# UI test: the cards of the Effects panel. One click picks a card: the Inspector shows its properties (nothing is added). "Preview" shows
# the effect on the Monitor without adding it; a double click adds it to the selected clip; picking a clip in the timeline brings back the
# clip's own Inspector. Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.   .\tools\uitest\pick_effects.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Pick.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    New-Sample "$work\talk.mp4" "--seconds 4 --height 360"
    & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
    [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\talk.mp4"; name = 'talk'; at = '0s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
    $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
    if (-not $r.ok) { throw "setup: $($r.error.message)" }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script @(
    'wait 1200'
    'click @clip:talk', 'wait 300'
    'click @rail:Effects', 'wait 500'
    'click @effect:blur', 'wait 600'
    'expect @check:pick_preview'              # the Inspector shows the picked effect
    'expect @button:pick_add_clip'
    'absent @effect:blur@2,2'                 # (a click alone added nothing: checked below on the project)
    "shot $work\picked.jpg"
    'click @check:pick_preview', 'wait 1500'  # preview on: the Monitor shows the blur
    "shot $work\previewed.jpg"
    'click @effect:vignette', 'wait 800'      # another card: the preview stays on and follows it
    'expect @check:pick_preview'
    "shot $work\previewed_vignette.jpg"
    'click @check:pick_preview', 'wait 1200'  # preview off: the film as it is
    "shot $work\preview_off.jpg"
    'dblclick @effect:sharpen', 'wait 1200'   # a double click adds it to the selected clip
    "shot $work\added.jpg"
    'click @rail:Effects', 'wait 300'
    'click @clip:talk', 'wait 600'            # a clip picked in the timeline: its own Inspector
    'absent @check:pick_preview'
    "shot $work\clip_again.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $tracks = @(Get-Tracks $run)
      $clip = Get-Object $run (($tracks | Where-Object { $_.name -like 'V*' -or $_.name -like 'Video*' } | Select-Object -First 1).clip_list[0].id)
      $kinds = @($clip.effects.PSObject.Properties | ForEach-Object { $_.Value.effect })
      "effects on the clip: $($kinds -join ', ')"
      if ($kinds.Count -ne 1 -or $kinds[0] -notlike 'attome.sharpen*') { $failed = "the clip should have only the sharpen that was double-clicked, it has: $($kinds -join ', ')" }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a click picks an effect (Inspector, preview, nothing added), a double click adds it, picking a clip brings its Inspector back (captures in $work)" -ForegroundColor Green
exit 0
