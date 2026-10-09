# UI test: a Library card is reached and added by key (UX review 6: W5, plan K4). F6 puts the keys in the Library panel, Tab goes from the
# search field to the card, Enter adds it at the playhead. The library is the test's own folder. Virtual input only (uitest.psm1).
#   .\tools\uitest\ux_card_keys.ps1


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

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
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
  'rclick @clip:wide', 'wait 400', 'click @menuitem:Add_to_the_library', 'wait 300', 'click @menuitem:Both', 'wait 900'
)
Stop-Daemon $runA
$runB = Invoke-EditorScript -Project $b -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'click @rail:Library', 'wait 900', 'expect @library:wide'
  'key F6', 'wait 300', 'key Tab', 'wait 300', "shot $work\ring_on_card.jpg"
  'key Enter', 'wait 1200'
  "shot $work\added.jpg"
)
$failed = $runB.Errors
try {
  if (-not $failed) {
    $n = 0; foreach ($t in @(Get-Tracks $runB)) { $n += @($t.clip_list).Count }
    "clips: $n"
    if ($n -ne 1) { $failed = "Enter on the card added $n clips, not 1" }
  }
} finally { Stop-Daemon $runB }
if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a Library card is reached by F6 and Tab and added by Enter; look at the captures in $work" -ForegroundColor Green
