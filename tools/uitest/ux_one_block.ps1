# UI test: a video with its sound is ONE clip on ONE track (the sound inside it, its waveform along the bottom). Its menu says "Detach audio": the sound
# becomes a clip of its own on an audio track, not linked. Undo gives the one clip back. Virtual input only (uitest.psm1).
# Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_one_block.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'OneBlock.attome'
if (Test-Path -LiteralPath $proj) { Remove-Item -LiteralPath $proj -Recurse -Force }
$env:ATTOME_PREF_DIR = Join-Path $work 'prefs'
New-Item -ItemType Directory -Force $env:ATTOME_PREF_DIR | Out-Null

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\v.mp4" "--seconds 4 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\v.mp4"; at = '0s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Shape($run) { # the tracks, and the picture and the sound clip if there are any
  $tracks = @(Get-Tracks $run)
  $v = ($tracks | Where-Object { $_.kind -eq 'video' }).clip_list
  $a = ($tracks | Where-Object { $_.kind -eq 'audio' }).clip_list
  [pscustomobject]@{ Tracks = $tracks.Count; Picture = $(if ($v) { Get-Object $run $v[0].id }); Sound = $(if ($a) { Get-Object $run $a[0].id }) }
}

$failed = $null
try {
  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
    'wait 1500'
    'click @clip:v', 'wait 400'
    "shot $work\one_block.jpg"
    'rclick @clip:v', 'wait 400', 'expect @menuitem:Detach_audio'
    'click @menuitem:Detach_audio', 'wait 900'
    "shot $work\detached.jpg"
  )
  $failed = $run.Errors
  if (-not $failed) {
    $x = Shape $run
    "after detaching: $($x.Tracks) tracks, picture stream '$($x.Picture.media_ref.stream)', sound stream '$($x.Sound.media_ref.stream)'"
    if ($x.Tracks -ne 2 -or -not $x.Sound -or $x.Sound.media_ref.stream -ne 'audio' -or $x.Picture.media_ref.stream -ne 'video') { $failed = 'Detach audio did not make a sound clip on an audio track and a silent picture' }
    elseif ($x.Picture.link_group -or $x.Sound.link_group) { $failed = 'the detached sound is still linked to its picture' }
  }
  if (-not $failed) { # Undo gives the one clip back
    $u = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl', 'wait 900', "shot $work\undone.jpg")
    $failed = $u.Errors
    if (-not $failed) {
      $x = Shape $run
      if ($x.Sound -or $x.Picture.media_ref.stream) { $failed = 'Undo did not give back the one clip with its sound' }
    }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = 'the project is not valid' }
  }
} finally {
  if ($run) { Stop-Daemon $run }
  Remove-Item Env:\ATTOME_PREF_DIR -ErrorAction SilentlyContinue
}

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a video is one clip with its sound; Detach audio makes the sound a clip of its own, Undo gives the one clip back (captures in $work)" -ForegroundColor Green
exit 0

