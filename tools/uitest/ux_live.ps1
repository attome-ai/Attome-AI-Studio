# UI test: values show while they are dragged (the owner's note: "should apply as I move them"). With the button held on a slider the
# Monitor already shows the change, and the project is not changed until it is let go: the Shared card's Scale on two titles, and a fade. Pictures are compared in the editor's own captures. Virtual input only (uitest.psm1). Exit code 0 = pass.
#   .\tools\uitest\ux_live.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Live.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(
        @{ op = 'add_text'; id = '$new:a'; text = 'One'; name = 'One'; at = '0s'; duration = '4s'; size = 0.1 },
        @{ op = 'add_text'; id = '$new:b'; text = 'Two'; name = 'Two'; at = '0s'; duration = '4s'; size = 0.1; position = @(0.5, 0.8); track = 'new'; track_name = 'More' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message) $($r.error.hint)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

Add-Type -AssemblyName System.Drawing
# Bright pixels in the Monitor's picture of a capture (the titles are white on black).
function Lit([string]$file) {
  $bmp = [System.Drawing.Bitmap]::FromFile($file)
  $n = 0
  for ($y = 110; $y -lt 490; $y += 2) { for ($x = 545; $x -lt 1220; $x += 2) { if ($bmp.GetPixel($x, $y).G -gt 200) { $n++ } } }
  $bmp.Dispose()
  $n
}
function Scale-Of($run, [string]$name) { foreach ($t in @(Get-Tracks $run)) { foreach ($c in @($t.clip_list)) { $o = Get-Object $run $c.id; if ($o.name -eq $name) { $s = $o.transform.scale; if ($s) { return $s[0] } else { return 1 } } } } }

$failed = $null
# One editor for each check: the button is held (the capture shows the Monitor then), and let go in the same script.
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 300 -Script @(
  'wait 1200'
  'key Home', 'key Right shift', 'wait 300'
  'click @clip:One', 'wait 300', 'click @clip:Two ctrl', 'wait 500'
  "shot $work\live_before.jpg"
  'drag @slider:multi_scale 60 0 hold', 'wait 700'
  "shot $work\live_held.jpg"
  'release', 'wait 900'
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $before = Lit "$work\live_before.jpg"; $held = Lit "$work\live_held.jpg"
    "lit pixels in the Monitor: $before before, $held with the Shared Scale held; saved after letting go: $(Scale-Of $run 'One'), $(Scale-Of $run 'Two')"
    if ($held -lt $before * 1.4) { $failed = 'the Monitor did not show the larger scale while the slider was held' }
    elseif ((Scale-Of $run 'One') -le 1 -or (Scale-Of $run 'Two') -le 1) { $failed = 'letting go did not save the scale on both' }
  }
  if (-not $failed) { # a fade in: the title dims at once while the slider is held (the playhead is 1 s in)
    $fd = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'key Home', 'key Right shift', 'wait 300', 'click @clip:One', 'wait 500'
      'click @button:add_card_fade', 'wait 400', "shot $work\live_fade_before.jpg"
      'drag @slider:fadein 120 0 hold', 'wait 700', "shot $work\live_fade_held.jpg", 'release', 'wait 900')
    $failed = $fd.Errors
    if (-not $failed) {
      $before = Lit "$work\live_fade_before.jpg"; $held = Lit "$work\live_fade_held.jpg"
      $keys = (Get-Object $run ((@(Get-Tracks $run) | Where-Object { $_.name -eq 'Titles' }).clip_list[0].id)).transform.keyframes.opacity
      "lit pixels: $before before the fade, $held with its slider held; fade keys saved after letting go: $(@($keys.PSObject.Properties).Count)"
      if ($held -gt $before * 0.8) { $failed = 'the fade did not show while its slider was held' }
      elseif (-not $keys) { $failed = 'letting go did not save the fade' }
    }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result | ConvertTo-Json -Compress -Depth 4)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the Shared Scale and a fade show in the Monitor while held, and are saved when let go (captures in $work)" -ForegroundColor Green
exit 0
