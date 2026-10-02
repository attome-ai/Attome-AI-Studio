# UI test: a video file and a sound file imported together land on a video track and an audio track; the Inspector's
# Audio card sets the sound clip's gain in dB and a fade in.
# Virtual input only (uitest.psm1). Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
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

# A video and a sound file dropped together; then the Audio card of the sound clip.
$run = Invoke-EditorScript -Project $proj -Import "$work\a.mp4", "$work\music.wav" -Script @(
  'click @clip:music.wav'
  'slide @slider:gain 0.5385'      # -40 .. +12 dB: about -12 dB
  'slide @slider:afadein 0.3333'   # 0 .. 6 s: about 2 s
  "shot $work\sound_set.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $tracks = @(Get-Tracks $run)
    $video = $tracks | Where-Object { $_.kind -eq 'video' }
    $audio = $tracks | Where-Object { $_.kind -eq 'audio' }
    "tracks: $(($tracks | ForEach-Object { "$($_.name)/$($_.kind)/$($_.clips)" }) -join ', ')"
    if (-not $video -or -not $audio -or $video.clips -ne 1 -or $audio.clips -ne 1) {
      $failed = 'the video and the sound file did not land on a video and an audio track'
    } else {
      $clip = Get-Object $run $audio.clip_list[0].id
      "audio: $($clip.audio | ConvertTo-Json -Compress)"
      $fade = ConvertFrom-Rational $clip.audio.fade_in
      if (-not ($clip.audio.gain_db -lt -10.5 -and $clip.audio.gain_db -gt -13.5)) { $failed = 'Gain did not become about -12 dB' }
      elseif (-not ($fade -gt 1.8 -and $fade -lt 2.2)) { $failed = 'Fade in did not become about 2 s' }
    }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: video and sound on their own tracks; gain and fade set from the Audio card (captures in $work)" -ForegroundColor Green
exit 0
