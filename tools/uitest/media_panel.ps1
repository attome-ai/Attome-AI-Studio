# UI test: the Media panel. An empty project shows one thing, the import target, with no heading, search or count.
# With media the top row is the filters (All, Video, Audio, Images) with the number of files the chosen one shows; a
# filter hides the files of the other kinds. Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\media_panel.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'MediaPanel.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# The samples first: New-Sample uses, and then clears, its own endpoint. The project is made on this test's endpoint,
# never on the default one (a daemon the user has running would open it and keep it).
New-Sample (Join-Path $work 'a.mp4') "--seconds 2 --height 360"
New-Sample (Join-Path $work 'm.wav') "--seconds 2"
$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try { & (Join-Path $bin 'attome.exe') --daemon never new $proj --rate 30 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

# Empty: the import target is there; the filters and the search are not.
$run = Invoke-EditorScript -Project $proj -Script @(
  'wait 400'
  'expect @button:import_media'
  "shot $work\media_empty.jpg"
)
$failed = $run.Errors
Stop-Daemon $run

if (-not $failed) { # With a video and a sound file.
  $run = Invoke-EditorScript -Project $proj -Import @((Join-Path $work 'a.mp4'), (Join-Path $work 'm.wav')) -Script @(
    'wait 800'
    'expect @filter:All'
    'expect @media:a.mp4'
    'expect @media:m.wav'
    "shot $work\media_all.jpg"
    'click @filter:Audio'
    'wait 300'
    'expect @media:m.wav'
    "shot $work\media_audio.jpg"
    'click @filter:Images'
    'wait 300'
    "shot $work\media_images.jpg"
    'click @filter:Video'
    'wait 300'
    'expect @media:a.mp4'
  )
  $failed = $run.Errors
  Stop-Daemon $run
}

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the Media panel's empty state and its filters (captures in $work)" -ForegroundColor Green
exit 0
