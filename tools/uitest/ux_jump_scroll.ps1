# UI test: a jump of the playhead by key (End, Home) brings it into view (UX review 6: W1). A long text clip; End is pressed,
# the capture must show the playhead and the clip's end in the lanes. Virtual input only (uitest.psm1). Look at the captures.
# Needs a build. Exit code 0 = the script ran; the captures are the check.
#   .\tools\uitest\ux_jump_scroll.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Jump.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_text'; text = 'Long'; name = 'Long'; at = '0s'; duration = '180s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$run = Invoke-EditorScript -Project $proj -SelectFirstClip -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'key End', 'wait 600'
  "shot $work\jump_end.jpg"
  'key Home', 'wait 600'
  "shot $work\jump_home.jpg"
)
try { } finally { Stop-Daemon $run }
if ($run.Errors) { Write-Host "FAIL: $($run.Errors)" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the playhead is in view after End and Home; look at the captures in $work" -ForegroundColor Green
exit 0
