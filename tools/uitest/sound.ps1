# UI test: a video file and a sound file imported together land on a video track and an audio track; the Inspector's
# Audio card sets the sound clip's gain in dB and a fade in.
# Virtual input only (uitest.psm1). Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\sound.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Sound.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 4 --height 540"
  New-Sample "$work\music.wav" "--seconds 6"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

# A video and a sound file dropped together; then the Audio card of the sound clip.
$run = Invoke-EditorScript -Project $proj -Import "$work\a.mp4", "$work\music.wav" -Script @(
  'click @clip:music'
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
    # The video's own sound is a linked clip on the audio track; the music follows it there.
    $music = if ($audio) { $audio.clip_list | Where-Object { $_.name -eq 'music' } } else { $null }
    if (-not $video -or -not $audio -or $video.clips -ne 1 -or $audio.clips -ne 2 -or -not $music) {
      $failed = 'expected the picture on a video track, and its sound plus the music on an audio track'
    } else {
      $picture = Get-Object $run $video.clip_list[0].id
      $sound = Get-Object $run ($audio.clip_list | Where-Object { $_.name -eq 'a' }).id
      "link: picture=$($picture.link_group) sound=$($sound.link_group) stream=$($picture.media_ref.stream)/$($sound.media_ref.stream)"
      if (-not $picture.link_group -or $picture.link_group -ne $sound.link_group) { $failed = 'the video and its sound are not linked' }
      $clip = Get-Object $run $music.id
      "audio: $($clip.audio | ConvertTo-Json -Compress)"
      $fade = ConvertFrom-Rational $clip.audio.fade_in
      if ($failed) { }
      elseif (-not ($clip.audio.gain_db -lt -10.5 -and $clip.audio.gain_db -gt -13.5)) { $failed = 'Gain did not become about -12 dB' }
      elseif (-not ($fade -gt 1.8 -and $fade -lt 2.2)) { $failed = 'Fade in did not become about 2 s' }
    }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a video's picture and linked sound, music on the audio track; gain and fade set from the Audio card (captures in $work)" -ForegroundColor Green
exit 0
