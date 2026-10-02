# UI test: a video file and a sound file imported together land on a video track and an audio track.
# Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\sound.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP 'attome-uitest'
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Sound.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" sample "$work\a.mp4" --seconds 4 --height 540 | Out-Null
  & "$bin\attome.exe" sample "$work\music.wav" --seconds 6 | Out-Null
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Get-Tracks($ed) { (Invoke-Attome $ed --json inspect $proj --level tracks | ConvertFrom-Json).result.data.sequences[0].tracks }

$ed = Start-Editor -Project $proj -Import "$work\a.mp4", "$work\music.wav"
$failed = $null
try {
  Show-Window $ed
  $tracks = @(Get-Tracks $ed)
  $video = $tracks | Where-Object { $_.kind -eq 'video' }
  $audio = $tracks | Where-Object { $_.kind -eq 'audio' }
  "tracks: $(($tracks | ForEach-Object { "$($_.name)/$($_.kind)/$($_.clips)" }) -join ', ')"
  Save-Shot $ed (Join-Path $work 'sound_imported.png') | Out-Null
  if (-not $video -or -not $audio -or $video.clips -ne 1 -or $audio.clips -ne 1) {
    $failed = 'the video and the sound file did not land on a video and an audio track'
  }
} finally { Stop-Editor $ed }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: video and sound imported to their own tracks (captures in $work)" -ForegroundColor Green
