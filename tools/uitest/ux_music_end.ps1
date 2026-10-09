# UI test: music longer than the video (UX review 4, P4). A 4 s video and 10 s of music: the music's Audio card says it plays 6 s
# past the end of the video and offers "End with the video", as does its menu; that ends it at 4 s with a 1.5 s fade out, in one
# edit, and then neither is offered. Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_music_end.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Music.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\v.mp4" "--seconds 4 --height 360"
  New-Sample "$work\music.wav" "--seconds 10"
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(
        @{ op = 'add_clip'; path = "$work\v.mp4"; name = 'v'; at = '0s'; with_audio = $false },
        @{ op = 'add_clip'; path = "$work\music.wav"; name = 'music'; at = '0s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message) $($r.error.hint)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Music($run) { foreach ($t in @(Get-Tracks $run)) { foreach ($c in @($t.clip_list)) { if ($c.name -eq 'music') { return Get-Object $run $c.id } } } }

$failed = $null
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script @(
  'wait 1200'
  'rclick @clip:music', 'wait 400', 'expect @menuitem:End_with_the_video', 'key Escape', 'wait 300'
  'click @clip:music', 'wait 500', 'expect @button:end_with_video'
  "shot $work\music_past_video.jpg"
  'click @button:end_with_video', 'wait 900'
  'absent @button:end_with_video'
  "shot $work\music_ended.jpg"
  'rclick @clip:music', 'wait 400', 'absent @menuitem:End_with_the_video', 'key Escape'
)
try {
  $failed = $run.Errors
  if (-not $failed) {
    $m = Music $run
    $len = ConvertFrom-Rational $m.timing.duration; $fade = ConvertFrom-Rational $m.audio.fade_out
    "music: $len s, fade out $fade s"
    if ([math]::Abs($len - 4) -gt 0.001) { $failed = "the music is $len s long, not 4" }
    elseif ([math]::Abs($fade - 1.5) -gt 0.001) { $failed = "the music fades out over $fade s, not 1.5" }
  }
  if (-not $failed) { # one undo brings back the whole of it
    $u = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('wait 600', 'key Z ctrl', 'wait 800')
    $failed = $u.Errors
    if (-not $failed) {
      $m = Music $run
      if ([math]::Abs((ConvertFrom-Rational $m.timing.duration) - 10) -gt 0.001 -or $m.audio.fade_out) { $failed = "one undo left the music at $($m.timing.duration) with fade $($m.audio.fade_out)" }
    }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: music past the video is offered End with the video, which ends it there with a fade, in one undo (captures in $work)" -ForegroundColor Green
exit 0
