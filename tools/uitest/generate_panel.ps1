# UI test: the Generate panel. A typed prompt becomes a generative clip on the timeline; a second one, with "Start from
# the clip before" ticked and "Add and generate" pressed, is linked to the first and both are generated (the first is
# needed and not made yet). Mock engine: no model, no GPU. Virtual input only (uitest.psm1). Exit code 0 = pass.
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
    'click @combo:gen_model'              # two models: the mock (ready) and the catalog's (not installed here)
    'wait 300'
    "shot $work\panel_models.jpg"
    'click @model:minimax-h3.fl2va.turbo8-int8'
    'wait 300'
    'expect @note:gen_model'              # the red note: not installed
    "shot $work\panel_not_installed.jpg"
    'click @combo:gen_model'
    'wait 300'
    'click @model:attome-mock'
    'wait 300'
    'click @field:gen_prompt'
    'type A robot walks through the snow'
    'wait 100'
    "shot $work\panel_typed.jpg"
    'click @button:gen_add'
    'wait 500'
    'click @field:gen_prompt'
    'type It finds a lantern, and lifts it'
    'click @check:gen_chain'
    'wait 100'
    'click @button:gen_add_run'
    'wait 300'
    'expect @button:gen_run'             # the new clip is selected; its card is back from "Stop" when the run has ended
    'wait 1500'
    "shot $work\panel_done.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $clips = @((Invoke-Attome $run --json gen status $proj | ConvertFrom-Json).result.clips)
      "clips: $(($clips | ForEach-Object { "'$($_.name)' $($_.state) takes=$($_.takes) depends=$(@($_.depends_on).Count)" }) -join '; ')"
      if ($clips.Count -ne 2) { $failed = "expected two generative clips, found $($clips.Count)" }
      elseif (@($clips | Where-Object { $_.state -ne 'clean' -or $_.takes -ne 1 }).Count) { $failed = 'both clips are not generated' }
      elseif ($clips[0].workflow -ne $clips[1].workflow) { $failed = 'the two clips do not share one Shot workflow' }
      elseif (@($clips[1].depends_on).Count -ne 1 -or $clips[1].depends_on[0] -ne $clips[0].clip) { $failed = 'the second clip does not start from the first' }
      else {
        $second = Get-Object $run $clips[1].clip
        "second clip: '$($second.name)' at $($second.timing.record_in) for $($second.timing.duration), $($second.media_ref.inputs.width) x $($second.media_ref.inputs.height)"
        if ($second.timing.record_in -ne '5' -or $second.timing.duration -ne '5') { $failed = 'the second clip is not at 5 s for 5 s' }
        elseif ([math]::Abs($second.transform.scale[0] - 1.0227) -gt 0.0002) { $failed = "the clip is not scaled to cover the canvas (scale $($second.transform.scale[0]))" }
        elseif ($second.media_ref.inputs.width -ne 1264 -or $second.media_ref.inputs.height -ne 704) { $failed = 'the size is not the canvas shape on the mock model''s grid of 16' }
      }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE, Env:\ATTOME_MODELS_DIR -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: two clips from the Generate panel, the second chained to the first, both generated (captures in $work)" -ForegroundColor Green
exit 0
