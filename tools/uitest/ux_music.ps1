# UI test: music goes under the picture (UX review 2: C4). A sound file added from the Media panel lands at the playhead on a
# sound track that is free there, or on a new one: not at the end of the track that holds the video's own sound.
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_music.ps1

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
  New-Sample "$work\vid.mp4" "--seconds 6 --height 360"
  New-Sample "$work\song.wav" "--seconds 12"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\vid.mp4"; at = '0s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Songs($run) {
  foreach ($t in @(Get-Tracks $run)) { foreach ($c in @($t.clip_list)) { $o = Get-Object $run $c.id; if ($o.name -eq 'song') { [pscustomobject]@{ track = $t.name; kind = $t.kind; at = [double](ConvertFrom-Rational $o.timing.record_in) } } } }
}

$failed = $null
# The song is imported with the project (it is added like a file dropped on the window), the playhead at the start.
$run = Invoke-EditorScript -Project $proj -Import @("$work\song.wav") -TimeoutSeconds 240 -Script @('wait 1500', "shot $work\music_first.jpg")
$failed = $run.Errors
try {
  if (-not $failed) {
    $songs = @(Songs $run)
    "first song: $($songs | ForEach-Object { "$($_.track) at $($_.at) s" })"
    if ($songs.Count -ne 1) { $failed = "expected one song clip, found $($songs.Count)" }
    elseif ($songs[0].kind -ne 'audio' -or $songs[0].track -eq 'A1') { $failed = "the song is on $($songs[0].track): it should have a sound track of its own, under the video's sound" }
    elseif ($songs[0].at -ne 0) { $failed = "the song starts at $($songs[0].at) s, not at the playhead (0 s)" }
  }
  if (-not $failed) { # again from its card with the playhead at 2 s: the first song is in the way there, so another track is made
    $again = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Home', 'key Right shift', 'key Right shift', 'wait 300', 'click @button:add_media_song.wav', 'wait 900', "shot $work\music_second.jpg")
    $failed = $again.Errors
    if (-not $failed) {
      $songs = @(Songs $run)
      "songs: $(($songs | ForEach-Object { "$($_.track) at $($_.at) s" }) -join ', ')"
      $second = $songs | Where-Object { $_.at -eq 2 }
      if ($songs.Count -ne 2 -or -not $second) { $failed = 'the second song was not put at the playhead (2 s)' }
      elseif (@($songs | Select-Object -ExpandProperty track -Unique).Count -ne 2) { $failed = 'both songs are on one track' }
    }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result | ConvertTo-Json -Compress -Depth 4)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a sound added from the panel lands at the playhead on a free sound track, or on a new one (captures in $work)" -ForegroundColor Green
exit 0
