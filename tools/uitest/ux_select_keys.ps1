# UI test: a clip is selected by key (UX review 6: W2). D selects the clip under the playhead; Tab and Shift+Tab the next and the
# previous clip on its track, with the playhead going to its start. Three text clips in a row on one track. Virtual input only.
#   .\tools\uitest\ux_select_keys.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Keys.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  $ops = @(foreach ($i in 0..2) { @{ op = 'add_text'; text = "T$i"; name = "T$i"; at = "$($i * 3)s"; duration = '2s' } })
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = $ops } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

# Delete after a selection shows which clip the key chose.
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'key Home', 'key Right shift', 'wait 300'      # 1 s: under T0
  'key D', 'wait 300', "shot $work\d.jpg", 'key Delete', 'wait 600'         # T0 goes
  'key Tab', 'wait 300', 'key Tab', 'wait 300'   # nothing selected: T1 (3 s), then T2 (6 s)
  "shot $work\tab.jpg"
  'key Tab', 'wait 300'                          # the last one: it stays selected
  'key Tab shift', 'wait 300'                    # back to T1
  'key Delete', 'wait 600'                       # T1 goes: only T2 is left
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $names = @(foreach ($t in @(Get-Tracks $run)) { foreach ($c in @($t.clip_list)) { $c.name } })
    "left: $($names -join ',')"
    if (($names -join ',') -ne 'T2') { $failed = "expected only T2 to be left, found: $($names -join ',')" }
  }
} finally { Stop-Daemon $run }
if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: D and Tab select clips (captures in $work)" -ForegroundColor Green
