# UI test: keyframes on a clip's opacity. The diamond beside Opacity puts a key at the playhead;
# a change of the value elsewhere adds a key there, so the clip grows between the two; the Inspector shows the value at the playhead;
# the diamond on a key takes it away, and the last one leaves its value as the plain value. Virtual input only (uitest.psm1).
# Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_opacity_keys.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'OpacityKeys.attome'
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
$run = Invoke-EditorScript -Project $proj -SelectFirstClip -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'key Home', 'wait 300'
  'expect @key:opacity'
  'click @key:opacity', 'wait 700'
  'key Right shift', 'key Right shift', 'wait 400'
  'click @number:opacity', 'wait 300', 'type 20', 'key Enter', 'wait 800'
  "shot $work\opacity_keys.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $k = Keys $run 'opacity'
    "opacity keys: $($k | ConvertTo-Json -Compress)"
    $static = (Clip $run).transform.opacity
    if ($k.Count -ne 2) { $failed = "expected two opacity keys, found $($k.Count)" }
    elseif ($k[0].t -ne '0' -or $k[0].v -ne 1) { $failed = 'the first key is not 100 % at the start' }
    elseif ([math]::Abs((ConvertFrom-Rational $k[1].t) - 2) -gt 0.01 -or [math]::Abs($k[1].v - 0.2) -gt 0.001) { $failed = 'the second key is not 20 % at 2 s' }
    elseif ($static -and $static -ne 1) { $failed = "typing at 2 s changed the plain opacity ($static) instead of adding a key" }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = 'the project is not valid' }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the diamond beside Opacity keys it, a value typed elsewhere adds a key there (captures in $work)" -ForegroundColor Green
exit 0
