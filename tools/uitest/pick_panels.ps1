# UI test: every tab of the left rail works the same way. One click on a card picks it and the Inspector shows its properties; nothing is
# added to the timeline. (A double click or a drag adds: the other tests.) Captures of each Inspector are the thing to look at.
# Mock engine, virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.   .\tools\uitest\pick_panels.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Panels.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
$settings = Join-Path $work 'settings.json'
Remove-Item $settings -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
$env:ATTOME_SETTINGS = $settings
$env:ATTOME_MODELS_DIR = Join-Path $work 'models'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    New-Sample "$work\talk.mp4" "--seconds 4 --height 360"
    & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
    [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\talk.mp4"; name = 'talk'; at = '0s' }, @{ op = 'add_clip'; path = "$work\talk.mp4"; name = 'two'; at = '5s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
    $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
    if (-not $r.ok) { throw "setup: $($r.error.message)" }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 300 -Script @(
    'wait 1200'
    'click @clip:talk', 'wait 300'
    # Text: a style, and Auto captions
    'click @rail:Text', 'wait 500', 'click @style:Neon', 'wait 500', 'expect @button:pick_add_style', "shot $work\text_style.jpg"
    'click @captions:auto', 'wait 500', 'expect @button:auto_captions', "shot $work\text_captions.jpg"
    # Media: a file
    'click @rail:Media', 'wait 500', 'click @media:talk.mp4', 'wait 500', 'expect @button:pick_add_media', "shot $work\media.jpg"
    # Effects: a transition
    'click @rail:Effects', 'wait 400', 'click @tab:Transitions', 'wait 400', 'click @transition:wipe', 'wait 500', 'expect @button:pick_add_transition', 'expect @slider:transition_seconds', "shot $work\transition.jpg"
    # Generate: a model
    'click @rail:Generate', 'wait 500', 'click @model:attome-mock', 'wait 500', 'expect @button:pick_add_model', "shot $work\model.jpg"
    # Cast: a new character
    'click @rail:Cast', 'wait 500', 'click @button:cast_new_character', 'wait 500', 'expect @field:cast_name', "shot $work\cast.jpg"
    # Niches: a niche, and the way to start one
    'click @rail:Niches', 'wait 500', 'click @niche:niche-skeleton', 'wait 500', 'expect @field:niche_text', "shot $work\niche.jpg"
    'click @button:niche_new', 'wait 500', 'expect @button:niche_new_default', "shot $work\niche_new.jpg"
    # Models: a model of the store, and the settings window
    'click @rail:Models', 'wait 800', 'click @appmodel:minimax-h3.fl2va.turbo8-int8', 'wait 500', 'expect @button:model_fetch', "shot $work\appmodel.jpg"
    'click @menu:File', 'wait 300', 'click @menuitem:Settings...', 'wait 900', 'expect @field:comfyui', 'expect @button:models_folder', "shot $work\settings.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) { # nothing was added by any of those clicks: the two clips of the setup are all there is
      $tracks = @(Get-Tracks $run)
      $clips = @($tracks | ForEach-Object { @($_.clip_list) })
      "tracks: $($tracks.Count); clips: $($clips.Count) ($($clips.name -join ', '))"
      if ($clips.Count -ne 2) { $failed = "a click added something: $($clips.name -join ', ')" }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE, Env:\ATTOME_SETTINGS, Env:\ATTOME_MODELS_DIR -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a click on a card in any panel shows its properties in the Inspector and adds nothing (captures in $work)" -ForegroundColor Green
exit 0
