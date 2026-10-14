# UI test: the Colour effects (black and white, sepia, invert, posterize, temperature and tint, exposure, highlights and shadows) are cards under
# the Colour tab; a double click puts one on the selected clip, and the Media card's menu keeps a file in the library. The library is the
# test's own folder. Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.   .\tools\uitest\colour_effects.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Colour.attome'
$lib = Join-Path $work 'library'
foreach ($p in $proj, $lib) { Remove-Item $p -Recurse -Force -ErrorAction SilentlyContinue }
$env:ATTOME_LIBRARY_DIR = $lib

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

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
    'wait 1200'
    'click @clip:talk', 'wait 300'
    'click @rail:Effects', 'wait 500'
    'click @tab:Colour', 'wait 400'
    'expect @effect:bw', 'expect @effect:sepia', 'expect @effect:invert', 'expect @effect:posterize'
    'expect @effect:temp', 'expect @effect:exposure', 'expect @effect:tones'
    'absent @effect:blur'                         # the Colour tab holds colour only
    "shot $work\colour_tab.jpg"
    'click @effect:tones', 'wait 600'             # one click shows its sliders in the Inspector
    'expect @check:pick_preview'
    "shot $work\tones_picked.jpg"
    'dblclick @effect:sepia', 'wait 1000'         # a double click adds it to the selected clip
    'click @tab:Detail', 'wait 300'
    'expect @effect:blur'
    'absent @effect:sepia'
    'click @rail:Media', 'wait 500'
    'rclick @media:talk.mp4', 'wait 400'          # the Media card's menu keeps the file in the library
    'click @menuitem:Add_to_the_library', 'wait 1500'
    "shot $work\media_menu.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $tracks = @(Get-Tracks $run)
      $clip = Get-Object $run (($tracks | Where-Object { $_.name -like 'V*' -or $_.name -like 'Video*' } | Select-Object -First 1).clip_list[0].id)
      $kinds = @($clip.effects.PSObject.Properties | ForEach-Object { $_.Value.effect })
      "effects on the clip: $($kinds -join ', ')"
      if ($kinds.Count -ne 1 -or $kinds[0] -notlike 'attome.sepia*') { $failed = "the clip should have only the sepia that was double-clicked, it has: $($kinds -join ', ')" }
      $items = @(Get-ChildItem (Join-Path $lib 'clips') -Directory -ErrorAction SilentlyContinue)
      "library items: $($items.Count)"
      if (-not $failed -and $items.Count -ne 1) { $failed = "the Media card's menu should have put 1 item in the library, there are $($items.Count)" }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the Colour tab holds the seven new effects, a click shows its sliders, a double click adds it, the Media card's menu keeps the file in the library (captures in $work)" -ForegroundColor Green
exit 0
