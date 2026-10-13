# UI test: every tab of the left rail, captured (to look at), with a clip on the timeline and the mock engine.
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.   .\tools\uitest\rail_tabs.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Tabs.attome'
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
    [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\talk.mp4"; name = 'talk'; at = '0s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
    $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
    if (-not $r.ok) { throw "setup: $($r.error.message)" }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $script = @('wait 1200')
  foreach ($tab in 'Media', 'Text', 'Effects', 'Generate', 'Library', 'Cast', 'Niches', 'Models') {
    $script += "click @rail:$tab", 'wait 700', "shot $work\tab_$($tab.ToLower()).jpg"
  }
  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script $script
  $failed = $run.Errors
  Stop-Daemon $run
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE, Env:\ATTOME_SETTINGS, Env:\ATTOME_MODELS_DIR -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: every rail tab captured in $work" -ForegroundColor Green
exit 0
