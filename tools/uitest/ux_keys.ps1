# UI test: keyframes on a clip's position, scale and rotation (UX review 2: B5). The diamond beside Scale puts a key at the playhead;
# a change of the value elsewhere adds a key there, so the clip grows between the two; the Inspector shows the value at the playhead;
# the diamond on a key takes it away, and the last one leaves its value as the plain value. Virtual input only (uitest.psm1).
# Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_keys.ps1

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
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_text'; text = 'Grow'; name = 'Grow'; at = '0s'; duration = '4s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Clip($run) { $t = @(Get-Tracks $run)[0]; Get-Object $run @($t.clip_list)[0].id }
function Keys($run, $prop) {
  $k = (Clip $run).transform.keyframes.$prop
  if (-not $k) { return ,@() }
  ,@($k.PSObject.Properties | ForEach-Object { $_.Value } | Sort-Object { [double](ConvertFrom-Rational $_.t) })
}

$failed = $null
# The playhead at the start: a key for the scale there; then at 2 s the scale is typed: 200 %, a second key.
$run = Invoke-EditorScript -Project $proj -SelectFirstClip -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'key Home', 'wait 300'
  'expect @key:scale', 'expect @key:position', 'expect @key:rotation'
  'click @key:scale', 'wait 700'
  'key Right shift', 'key Right shift', 'wait 400'
  'click @number:scale', 'wait 300', 'type 200', 'key Enter', 'wait 800'
  'key Left shift', 'wait 500'                     # at 1 s, half way
  "shot $work\keys_halfway.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $k = Keys $run 'scale'
    "scale keys: $($k | ConvertTo-Json -Compress)"
    $static = (Clip $run).transform.scale
    if ($k.Count -ne 2) { $failed = "expected two scale keys, found $($k.Count)" }
    elseif ($k[0].t -ne '0' -or $k[0].v[0] -ne 1) { $failed = 'the first key is not 100 % at the start' }
    elseif ([math]::Abs((ConvertFrom-Rational $k[1].t) - 2) -gt 0.01 -or $k[1].v[0] -ne 2) { $failed = 'the second key is not 200 % at 2 s' }
    elseif ($static -and $static[0] -ne 1) { $failed = "typing at 2 s changed the plain scale ($($static[0])) instead of adding a key" }
  }
  if (-not $failed) { # the picture: the text at 1 s is about 150 % of its size at the start (the Monitor's renderer reads the keys)
    [IO.File]::WriteAllText("$work\see.json", (@{ project = $proj; times = @('0s', '1s', '2s'); height = 360 } | ConvertTo-Json -Compress), (New-Object Text.UTF8Encoding($false)))
    $see = (Invoke-Attome $run --json call see.frames "$work\see.json" | ConvertFrom-Json).result
    Add-Type -AssemblyName System.Drawing
    $widths = foreach ($img in $see.images) {
      $bmp = [System.Drawing.Bitmap]::FromFile($img.path)
      $minx = $bmp.Width; $maxx = 0
      for ($x = 0; $x -lt $bmp.Width; $x += 2) { for ($y = 120; $y -lt 240; $y += 3) { if ($bmp.GetPixel($x, $y).G -gt 128) { if ($x -lt $minx) { $minx = $x }; if ($x -gt $maxx) { $maxx = $x } } } }
      $bmp.Dispose()
      [math]::Max(0, $maxx - $minx)
    }
    "text width at 0, 1 and 2 s: $($widths -join ', ') px"
    if (-not ($widths[1] -gt $widths[0] * 1.3 -and $widths[2] -gt $widths[1] * 1.15)) { $failed = 'the text does not grow from the first key to the second' }
  }
  if (-not $failed) { # on the key at 2 s the diamond takes it away; at the start the last one goes and leaves 100 %
    $off = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @(
      'key Home', 'key Right shift', 'key Right shift', 'wait 400', 'click @key:scale', 'wait 700'
      'key Home', 'wait 400', 'click @key:scale', 'wait 700')
    $failed = $off.Errors
    if (-not $failed) {
      $k = Keys $run 'scale'
      $static = (Clip $run).transform.scale
      "after removing both: $($k.Count) keys, plain scale $($static -join ' x ')"
      if ($k.Count -ne 0) { $failed = "$($k.Count) scale keys are left" }
      elseif ($static[0] -ne 1) { $failed = "the plain scale is $($static[0]), not 1" }
    }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result | ConvertTo-Json -Compress -Depth 4)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a key from the diamond, a second from a typed value at another time, the clip grows between them, and the keys come off (captures in $work)" -ForegroundColor Green
exit 0
