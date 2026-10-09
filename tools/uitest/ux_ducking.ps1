# UI test: ducking (UX review 4, P3). A 4 s talking clip with its sound and 8 s of music, both from 0. The music's Audio card has
# "Lower under the voices": the music goes 12 dB down while the talk plays and back up after it (keys of its level). Its Gain
# then moves the keys with it, "Again" and "Remove" are offered, and Remove takes the keys away. Virtual input only (uitest.psm1).
# Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_ducking.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Duck.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\talk.mp4" "--seconds 4 --height 360"
  New-Sample "$work\music.wav" "--seconds 8"
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(
        @{ op = 'add_clip'; path = "$work\talk.mp4"; name = 'talk'; at = '0s'; separate_audio = $true },
        @{ op = 'add_track'; id = '$new:mt'; kind = 'audio'; name = 'Music' },
        @{ op = 'add_clip'; path = "$work\music.wav"; name = 'music'; at = '0s'; track = '$new:mt' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message) $($r.error.hint)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Music($run) { foreach ($t in @(Get-Tracks $run)) { foreach ($c in @($t.clip_list)) { if ($c.name -eq 'music') { return Get-Object $run $c.id } } } }
# The music's level keys as "t:v" (seconds:dB), in time order.
function Keys($run) {
  $m = Music $run
  if (-not $m.audio.keyframes -or -not $m.audio.keyframes.gain_db) { return '' }
  (@($m.audio.keyframes.gain_db.PSObject.Properties | ForEach-Object { [pscustomobject]@{ T = (ConvertFrom-Rational $_.Value.t); V = [double]$_.Value.v } } |
      Sort-Object T | ForEach-Object { '{0:0.##}:{1:0.#}' -f $_.T, $_.V })) -join ' '
}

$failed = $null
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script @(
  'wait 1200'
  'click @clip:music', 'wait 500', 'expect @button:duck', 'absent @button:duck_off'
  'click @button:duck', 'wait 900'
  'expect @button:duck_again', 'expect @button:duck_off'
  "shot $work\ducked.jpg"
)
try {
  $failed = $run.Errors
  if (-not $failed) {
    $k = Keys $run
    "keys after ducking: $k"
    # down to -12 from 0 to 4 s (the talk), back to 0 a ramp after it
    if ($k -notmatch '^0:-12 4(\.0\d?)?:-12 4\.1\d?:0$') { $failed = "the music's keys are '$k', not down 12 dB under the talk (0..4 s) and back after" }
  }
  if (-not $failed) { # Gain moves the keys with it
    $g = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('wait 600', 'click @clip:music', 'wait 400', 'click @number:gain', 'wait 200', 'type -6', 'key Enter', 'wait 900')
    $failed = $g.Errors
    if (-not $failed) {
      $k = Keys $run
      "keys after Gain -6: $k"
      if ($k -notmatch '^0:-18 4(\.0\d?)?:-18 4\.1\d?:-6$') { $failed = "after Gain -6 the keys are '$k', not moved 6 dB down" }
    }
  }
  if (-not $failed) { # Remove takes the keys away
    $o = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('wait 600', 'click @clip:music', 'wait 400', 'click @button:duck_off', 'wait 900', 'expect @button:duck')
    $failed = $o.Errors
    if (-not $failed -and (Keys $run)) { $failed = "Remove left keys: $(Keys $run)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the music ducks under the talk, Gain moves its keys, Remove takes them away (captures in $work)" -ForegroundColor Green
exit 0
