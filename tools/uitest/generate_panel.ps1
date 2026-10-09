# UI test: the Generate panel. A click on a model's card adds a generative clip whose prompt is typed in the Inspector; a
# second one, set to start from the clip before on its own card, is linked to the first, and Generate on it makes both (the first
# is needed and not made yet). Mock engine: no model, no GPU. Virtual input only (uitest.psm1). Exit code 0 = pass.
#   .\tools\uitest\generate_panel.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Panel.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
$env:ATTOME_MODELS_DIR = Join-Path $work 'models' # empty: the catalog's model is listed, not installed; the mock is ready
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try { & "$bin\attome.exe" new $proj --rate 24 --canvas 1280x720 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -Script @(
    'click @rail:Generate'
    'wait 400'
    'expect @model:minimax-h3.fl2va.turbo8-int8' # two cards: the mock (ready) and the catalog's (not installed: a badge)
    "shot $work\panel_models.jpg"
    'click @model:attome-mock'            # a click adds the clip at the playhead and selects it; its prompt is in the Inspector
    'wait 500'
    'click @field:prompt'
    'type A robot walks through the snow'
    'wait 100'
    "shot $work\panel_typed.jpg"
    'click @rail:Generate'                # leaving the field saves the prompt
    'wait 400'
    'click @model:attome-mock'
    'wait 500'
    'click @field:prompt'
    'type It finds a lantern, and lifts it'
    'click @rail:Generate'                # leaves the field
    'wait 400'
    'click @combo:gen_start'              # the clip's own setting: it starts from the last frame of the clip before
    'click @option:gen_start_previous'
    'wait 400'
    'expect @slider:gen_length'           # its length is on its card too
    'click @button:gen_run'               # the second clip: the first is made too, because the second starts from it
    'wait 300'
    'expect @button:gen_run'             # the new clip is selected; its card is back from "Stop" when the run has ended
    'wait 1500'
    "shot $work\panel_done.jpg"
    'click @clip:Shot_1'                 # the pointer rests on the clip: its prompt shows as a tooltip
    'wait 900'
    "shot $work\panel_tooltip.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $clips = @((Invoke-Attome $run --json gen status $proj | ConvertFrom-Json).result.clips)
      "clips: $(($clips | ForEach-Object { "'$($_.name)' $($_.state) takes=$($_.takes) depends=$(@($_.depends_on).Count)" }) -join '; ')"
      if ($clips.Count -ne 2) { $failed = "expected two generative clips, found $($clips.Count)" }
      elseif ($clips[0].name -ne 'Shot 1' -or $clips[1].name -ne 'Shot 2') { $failed = 'the clips are not named Shot 1 and Shot 2' }
      elseif (@($clips | Where-Object { $_.state -ne 'clean' -or $_.takes -ne 1 }).Count) { $failed = 'both clips are not generated' }
      elseif ($clips[0].source -ne $clips[1].source -or -not $clips[0].source) { $failed = 'the two clips were not copied from the same Clip Workflow' }
      elseif (@($clips[1].depends_on).Count -ne 1 -or $clips[1].depends_on[0] -ne $clips[0].clip) { $failed = 'the second clip does not start from the first' }
      else {
        $second = Get-Object $run $clips[1].clip
        "second clip: '$($second.name)' at $($second.timing.record_in) for $($second.timing.duration), $($second.media_ref.width) x $($second.media_ref.height)"
        if ($second.timing.record_in -ne '5' -or $second.timing.duration -ne '5') { $failed = 'the second clip is not at 5 s for 5 s' }
        elseif ([math]::Abs($second.transform.scale[0] - 1.0127) -gt 0.0002) { $failed = "the clip is not scaled to cover the canvas (scale $($second.transform.scale[0]))" }
        elseif ($second.media_ref.width -ne 1264 -or $second.media_ref.height -ne 720) { $failed = 'the size is not the canvas shape on the mock model''s grid of 16' }
      }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE, Env:\ATTOME_MODELS_DIR -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: two clips from the Generate panel, the second chained to the first, both generated (captures in $work)" -ForegroundColor Green
exit 0
