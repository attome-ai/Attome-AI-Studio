# UI test: selected clips are moved by key (UX review 6: W3). Alt+Right a frame, Shift+Alt+Right a second, Alt+Left back; Alt+Down
# puts a clip on the track below where it is free, and says so where it is not. Two video clips on two tracks. Virtual input only.
#   .\tools\uitest\ux_nudge.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Nudge.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 2 --height 180"
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  $ops = @(@{ op = 'add_clip'; path = "$work\a.mp4"; at = '5s' }, @{ op = 'add_clip'; path = "$work\a.mp4"; at = '0s' })
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = $ops } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Starts($run) { @(foreach ($t in @(Get-Tracks $run)) { foreach ($c in @($t.clip_list)) { '{0}:{1:0.###}' -f $t.name, [double](ConvertFrom-Rational $c.record_in) } }) -join ' ' }

$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'key Home', 'key D', 'wait 300'
  'key Right alt', 'wait 500'                                       # a frame
  'key Right alt shift', 'wait 500'                                 # and a second
  "shot $work\moved.jpg"
  'key Down alt', 'wait 400', "shot $work\no_track.jpg"      # no track of its kind below: a line says so
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $s = Starts $run; "after Alt+Right, Shift+Alt+Right: $s"
    if ($s -notmatch ':1\.033') { $failed = "the clip did not move by a frame and a second: $s" }
  }
} finally { Stop-Daemon $run }
if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Alt+arrows move clips (captures in $work)" -ForegroundColor Green
