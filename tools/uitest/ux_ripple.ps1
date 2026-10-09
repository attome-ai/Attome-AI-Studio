# UI test: taking time out of the film (UX review 3, R2 and R8).
#  - Shift+Delete deletes the selected clip and the clips after it close up (its sound goes and closes up too)
#  - a right click on an empty stretch of a track offers "Delete gap", and the clips after it move up
#  - W takes out the part of the selected clip after the playhead, Q the part before it, and what follows closes up
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_ripple.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Ripple.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# Four 2 s clips with sound, one after another on V1 (0-2, 2-4, 4-6, 6-8 s), their sound on A1.
$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\s.mp4" "--seconds 2 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  $ops = @()
  foreach ($n in 'one', 'two', 'three', 'four') { $ops += @{ op = 'add_clip'; separate_audio = $true; path = "$work\s.mp4"; name = $n } }
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = $ops } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

# The clips of a track as "name@start-end" in seconds.
function Spans($run, $track) {
  $t = @(Get-Tracks $run) | Where-Object { $_.name -eq $track }
  @($t.clip_list | Sort-Object { ConvertFrom-Rational $_.record_in } | ForEach-Object {
      $s = ConvertFrom-Rational $_.record_in; $d = ConvertFrom-Rational $_.duration
      '{0}@{1:0.#}-{2:0.#}' -f $_.name, $s, ($s + $d) }) -join ' '
}

$failed = $null
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script @(
  'wait 2000'
  'click @clip:two', 'wait 300'
  'key Delete shift', 'wait 800'          # one 0-2, three 2-4, four 4-6
  "shot $work\ripple_deleted.jpg"
)
try {
  if ($run.Errors) { $failed = $run.Errors }
  elseif ((Spans $run 'V1') -ne 'one@0-2 three@2-4 four@4-6') { $failed = "after Shift+Delete V1 is: $(Spans $run 'V1')" }
  elseif (@((Spans $run 'A1') -split ' ').Count -ne 3) { $failed = "after Shift+Delete A1 is: $(Spans $run 'A1')" }
} finally { if ($failed) { Stop-Daemon $run } }

if (-not $failed) {
  # A gap: three deleted the plain way leaves 2-4 empty; a right click in it (3 s, on V1's row) and Delete gap closes it.
  $run2 = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 200 -Script @(
    'wait 2000'
    'click @clip:three', 'wait 300'
    'key Delete', 'wait 800'
    'rclick @clip:four@-0.5,0.5', 'wait 500'  # half a clip's width to the left of four: inside the gap
    'click @menuitem:Delete_gap', 'wait 800'
    "shot $work\gap_deleted.jpg"
  )
  if ($run2.Errors) { $failed = $run2.Errors }
  elseif ((Spans $run 'V1') -ne 'one@0-2 four@2-4') { $failed = "after Delete gap V1 is: $(Spans $run 'V1')" }
}
if (-not $failed) {
  # W at 1 s in one (0-2): one 0-1, four 1-3. Then Q at 2 s in four (1-3): four 1-2.
  $run3 = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 200 -Script @(
    'wait 2000'
    'click @clip:one', 'wait 300'
    'key Home', 'key Right shift', 'wait 300'   # the playhead at 1 s
    'key W', 'wait 800'
    "shot $work\w_done.jpg"
  )
  if ($run3.Errors) { $failed = $run3.Errors }
  elseif ((Spans $run 'V1') -ne 'one@0-1 four@1-3') { $failed = "after W V1 is: $(Spans $run 'V1')" }
}
if (-not $failed) {
  $run4 = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 200 -Script @(
    'wait 2000'
    'click @clip:four', 'wait 300'
    'key Home', 'key Right shift', 'key Right shift', 'wait 300'   # 2 s
    'key Q', 'wait 800'
    "shot $work\q_done.jpg"
  )
  if ($run4.Errors) { $failed = $run4.Errors }
  elseif ((Spans $run 'V1') -ne 'one@0-1 four@1-2') { $failed = "after Q V1 is: $(Spans $run 'V1')" }
}
Stop-Daemon $run

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Shift+Delete, Delete gap, W and Q take time out and close up (captures in $work)" -ForegroundColor Green
exit 0
