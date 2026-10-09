# UI test: a sound's level animated, and its fades from its menu (UX review 4, P5 and P7). On a music clip's Audio card the diamond
# after Gain puts a key of the level at the playhead; with keys, a change of Gain sets the key at the playhead (a new one where
# there is none); the diamond on a key takes it away. The clip's menu has "Fade in and out", a second each. Virtual input only
# (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_sound_keys.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Keys.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\music.wav" "--seconds 6"
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\music.wav"; name = 'music'; at = '0s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message) $($r.error.hint)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Music($run) { foreach ($t in @(Get-Tracks $run)) { foreach ($c in @($t.clip_list)) { if ($c.name -eq 'music') { return Get-Object $run $c.id } } } }
function Keys($run) { # the level's keys as "t:v" (seconds:dB), in time order
  $m = Music $run
  if (-not $m.audio.keyframes -or -not $m.audio.keyframes.gain_db) { return '' }
  (@($m.audio.keyframes.gain_db.PSObject.Properties | ForEach-Object { [pscustomobject]@{ T = (ConvertFrom-Rational $_.Value.t); V = [double]$_.Value.v } } |
      Sort-Object T | ForEach-Object { '{0:0.##}:{1:0.#}' -f $_.T, $_.V })) -join ' '
}

$failed = $null
# A key at 0 s (the level as it is, 0 dB), then at 2 s Gain -10 makes a second key there.
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script @(
  'wait 1200'
  'click @clip:music', 'wait 400', 'key Home', 'wait 300'
  'click @key:gain_db', 'wait 800'
  'key Right shift', 'key Right shift', 'wait 400'
  'click @number:gain', 'wait 200', 'type -10', 'key Enter', 'wait 900'
  "shot $work\gain_keys.jpg"
)
try {
  $failed = $run.Errors
  if (-not $failed -and (Keys $run) -ne '0:0 2:-10') { $failed = "the level's keys are '$(Keys $run)', not 0:0 2:-10" }
  if (-not $failed) { # the diamond on the key at 2 s takes it away; the last key's level stays as the plain one
    $d = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('wait 600', 'click @clip:music', 'wait 400', 'key Home', 'key Right shift', 'key Right shift', 'wait 400', 'click @key:gain_db', 'wait 800')
    $failed = $d.Errors
    if (-not $failed -and (Keys $run) -ne '0:0') { $failed = "after the diamond on the key at 2 s the keys are '$(Keys $run)', not 0:0" }
  }
  if (-not $failed) { # P7: the menu's Fade in and out
    $f = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('wait 600', 'rclick @clip:music', 'wait 400', 'click @menuitem:Fade_in_and_out', 'wait 900')
    $failed = $f.Errors
    if (-not $failed) {
      $a = (Music $run).audio
      "fades: in $($a.fade_in), out $($a.fade_out)"
      if ((ConvertFrom-Rational $a.fade_in) -ne 1 -or (ConvertFrom-Rational $a.fade_out) -ne 1) { $failed = "the menu's fades are $($a.fade_in) and $($a.fade_out), not a second each" }
    }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Gain keys from the diamond and the slider, a key taken away, and a sound's fades from its menu (captures in $work)" -ForegroundColor Green
exit 0
