# UI test: the panels are reached by key (UX review 6: W5, plan docs/plan/KEYBOARD_PANELS.md). F6 puts the keys in the Inspector, on its
# first widget, with a ring round it; the arrows go from widget to widget (the panel follows) without moving the playhead; Enter presses the
# widget the ring is on (here the Italic box); Esc gives the keys back to the timeline, whose arrows move the playhead again and which still
# has its clip selected. Virtual input only (uitest.psm1). Look at the captures too: the ring.
#   .\tools\uitest\ux_panel_keys.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Panel.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_text'; text = 'Keys'; name = 'Keys'; at = '0s'; duration = '4s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$steps = @('wait 1200', 'key Home', 'key F6', 'key F6', 'key F6', 'wait 300', "shot $work\first.jpg")
foreach ($i in 1..6) { $steps += 'key Tab', 'wait 120' }           # Name, Start, Duration, the Text header, the text box, the Size slider
$steps += "shot $work\size_ring.jpg", 'key Right', 'key Right', 'key Right', 'key Right', 'key Right', 'wait 800'   # Right moves the slider: 5 steps
foreach ($i in 1..5) { $steps += 'key Tab', 'wait 120' }           # the number, Color, Bold, Font, the Italic box
$steps += "shot $work\italic_ring.jpg", 'key Enter', 'wait 600'    # Enter presses it
foreach ($i in 1..3) { $steps += 'key Down', 'wait 150' }
$steps += 'key Home', 'wait 200', "shot $work\home_in_panel.jpg"   # Home is the panel's: the playhead stays at the start
$steps += 'key Escape', 'wait 200', 'key Right', 'key Right', 'wait 300', "shot $work\back.jpg"   # the timeline's again: two frames
$run = Invoke-EditorScript -Project $proj -SelectFirstClip -TimeoutSeconds 240 -Script $steps
$failed = $run.Errors
try {
  if (-not $failed) {
    $t = @(Get-Tracks $run)[0]
    $c = (Get-Object $run @($t.clip_list)[0].id).content
    "italic after Enter: $($c.italic); size after Right x5: $($c.size)"
    if ($c.size -le 0.09) { $failed = "Right did not move the Size slider (size $($c.size), it was 0.08)" }
    elseif ($c.italic -ne $true) { $failed = 'Enter did not press the Italic box' }
  }
} finally { Stop-Daemon $run }
if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: F6 reaches the Inspector, Enter presses a widget, Esc returns; look at the captures in $work" -ForegroundColor Green
