# UI test: a library card shows the clip's own picture, not the canvas (UX review 6: W7), and a double click selects what it added and
# moves the playhead to its end (W6), so cards clicked in a row follow one another. A 16:9 clip kept from a 9:16 project, then used in
# another 9:16 project twice. The library is the test's own folder. Virtual input only (uitest.psm1). Look at the captures.
#   .\tools\uitest\ux_library_follow.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$a = Join-Path $work 'A.attome'; $b = Join-Path $work 'B.attome'
$lib = Join-Path $work 'library'
foreach ($p in $a, $b, $lib) { Remove-Item $p -Recurse -Force -ErrorAction SilentlyContinue }
$env:ATTOME_LIBRARY_DIR = $lib

$env:ATTOME_ENDPOINT = "\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\wide.mp4" "--seconds 3 --height 360"
  & "$bin\attome.exe" new $a --rate 30 --canvas 360x640 | Out-Null
  & "$bin\attome.exe" new $b --rate 30 --canvas 360x640 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $a; ops = @(@{ op = 'add_clip'; path = "$work\wide.mp4"; at = '0s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$runA = Invoke-EditorScript -Project $a -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'rclick @clip:wide', 'wait 400', 'click @menuitem:Add_both_to_the_library', 'wait 900'
)
Stop-Daemon $runA
if ($runA.Errors) { Write-Host "FAIL: $($runA.Errors)" -ForegroundColor Red; exit 1 }
$runB = Invoke-EditorScript -Project $b -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'click @rail:Library', 'wait 900', 'expect @library:wide'
  "shot $work\card.jpg"
  'dblclick @library:wide', 'wait 1200'
  'dblclick @library:wide', 'wait 1200'
  "shot $work\twice.jpg"
)
$failed = $runB.Errors
try {
  if (-not $failed) {
    $t = @(Get-Tracks $runB | Where-Object { @($_.clip_list).Count -gt 0 })
    $starts = @($t | ForEach-Object { $_.clip_list } | ForEach-Object { [double](ConvertFrom-Rational $_.record_in) } | Sort-Object)
    "clip starts: $($starts -join ', ')"
    if ($starts.Count -ne 2 -or [math]::Abs($starts[0]) -gt 0.01 -or [math]::Abs($starts[1] - 3) -gt 0.05) { $failed = 'the second clip does not follow the first at 3 s' }
  }
} finally { Stop-Daemon $runB }
if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: cards follow one another; look at the captures in $work" -ForegroundColor Green
