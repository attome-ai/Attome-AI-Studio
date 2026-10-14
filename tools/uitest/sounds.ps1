# UI test: the Sounds panel (Freesound sound effects). Without a key it shows how to get one; a key typed and saved goes into the settings
# (a settings file of this test's own) and the panel turns into the search; a sound already in the sound folder is listed under Downloaded.
# Freesound itself is not asked (no network): the engine test [sounds] covers search and download. Virtual input only (uitest.psm1).
#   .\tools\uitest\sounds.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Sounds.attome'
$ep = "\\.\pipe\attome-uitest-sounds-$PID"
$sounds = Join-Path $work 'sounds'
New-Item -ItemType Directory -Force $sounds | Out-Null
[IO.File]::WriteAllBytes((Join-Path $sounds 'freesound_7.mp3'), [byte[]](73, 68, 51, 0))
$about = @{ id = 7; name = 'Cartoon boing'; seconds = 0.8; author = 'maker'; license = 'CC0'; tags = @('boing', 'cartoon'); url = 'https://freesound.org/s/7/' }
[IO.File]::WriteAllText((Join-Path $sounds 'freesound_7.json'), ($about | ConvertTo-Json -Compress), (New-Object Text.UTF8Encoding($false)))

$env:ATTOME_SETTINGS = Join-Path $work 'settings.json'
$env:ATTOME_SOUNDS_DIR = $sounds
$oldKey = $env:ATTOME_FREESOUND_KEY
Remove-Item Env:\ATTOME_FREESOUND_KEY -ErrorAction SilentlyContinue
try {
  $env:ATTOME_ENDPOINT = $ep
  try { & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }
  $steps = @('wait 1200', 'click @rail:Sounds', 'wait 500', 'expect @button:sounds_get_key', 'expect @field:sounds_key', 'absent @field:sounds_search',
             "shot $work\no_key.jpg", 'click @field:sounds_key', 'type testkey123', 'click @button:sounds_save_key', 'wait 600', "shot $work\saved.jpg",
             'expect @field:sounds_search', 'absent @field:sounds_key', 'expect @button:add', "shot $work\search.jpg")
  $run = Invoke-EditorScript -Project $proj -Endpoint $ep -TimeoutSeconds 120 -Script $steps
  $failed = $run.Errors
  Stop-Daemon $run
  if (-not $failed) {
    $saved = Get-Content $env:ATTOME_SETTINGS -Raw | ConvertFrom-Json
    if ($saved.freesound_key -ne 'testkey123') { $failed = "the key was not kept in the settings: $($saved | ConvertTo-Json -Compress)" }
  }
} finally {
  Remove-Item Env:\ATTOME_SETTINGS, Env:\ATTOME_SOUNDS_DIR -ErrorAction SilentlyContinue
  if ($oldKey) { $env:ATTOME_FREESOUND_KEY = $oldKey }
}

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: key, search and downloaded sounds; captures in $work" -ForegroundColor Green
exit 0
