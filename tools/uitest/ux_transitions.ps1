# UI test: the Transitions tab of the Effects panel (UX review U14). A card is dragged onto the cut between two clips and a
# transition is put there; a click puts one on the cut after the selected clip; a second one on the same cut is refused; undo takes
# them back. Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_transitions.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Transitions.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# Three pictures of 2 s, one after the other, cut from files that are longer, so a transition has media to use.
$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 8 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  $ops = @(
    @{ op = 'add_clip'; id = '$new:a'; path = "$work\a.mp4"; at = '0s'; duration = '2s'; source_in = '2s'; name = 'First'; with_audio = $false },
    @{ op = 'add_clip'; id = '$new:b'; path = "$work\a.mp4"; at = '2s'; duration = '2s'; source_in = '2s'; name = 'Second'; with_audio = $false; track = '$new:a.track' },
    @{ op = 'add_clip'; id = '$new:c'; path = "$work\a.mp4"; at = '4s'; duration = '2s'; source_in = '2s'; name = 'Third'; with_audio = $false; track = '$new:a.track' })
  # the first clip makes the track the other two use
  $ops[0].track = 'new'
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = $ops } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message) $($r.error.hint)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Transitions($run) {
  $t = @(Get-Tracks $run) | Where-Object { @($_.clip_list).Count -ge 3 } | Select-Object -First 1
  $o = Get-Object $run $t.id
  if ($o.PSObject.Properties.Name -contains 'transitions' -and $o.transitions) { @($o.transitions.PSObject.Properties | ForEach-Object { $_.Value }) } else { @() }
}

$failed = $null
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
  'wait 1500'
  'click @rail:Effects', 'wait 400'
  'click @tab:Transitions', 'wait 500'
  'expect @transition:dissolve', 'expect @transition:iris'
  "shot $work\transitions_panel.jpg"
  'drag @transition:dissolve @clip:Second@0.03,0.5', 'wait 900'      # onto the cut between First and Second
  "shot $work\transitions_dropped.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $t = @(Transitions $run)
    "transitions after the drop: $($t.Count) $(($t | ForEach-Object { $_.type }) -join ',')"
    if ($t.Count -ne 1 -or $t[0].type -ne 'attome.dissolve') { $failed = "the drop did not make one dissolve ($($t.Count) transitions)" }
  }
  if (-not $failed) { # a click: on the cut after the selected clip
    $r2 = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('click @rail:Effects', 'click @tab:Transitions', 'wait 300', 'click @clip:Second', 'wait 400', 'click @transition:push', 'wait 900', 'expect @button:toast_undo')
    $failed = $r2.Errors
    if (-not $failed) {
      $t = @(Transitions $run)
      "transitions after the click: $($t.Count) $(($t | ForEach-Object { $_.type }) -join ',')"
      if ($t.Count -ne 2) { $failed = "the click did not add a push ($($t.Count) transitions)" }
    }
  }
  if (-not $failed) { # the same cut again: refused, nothing changes
    $r3 = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('click @rail:Effects', 'click @tab:Transitions', 'wait 300', 'click @clip:First', 'wait 400', 'click @transition:wipe', 'wait 700', 'expect @toast:There_is_a_transition_on')
    $failed = $r3.Errors
    if (-not $failed -and @(Transitions $run).Count -ne 2) { $failed = 'a second transition went onto a cut that had one' }
  }
  if (-not $failed) { # undo takes the last one back
    $r4 = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl', 'wait 800')
    $failed = $r4.Errors
    if (-not $failed -and @(Transitions $run).Count -ne 1) { $failed = "undo left $(@(Transitions $run).Count) transitions" }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result.problems | ConvertTo-Json -Compress)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a transition card dragged onto a cut, or clicked after the selected clip, makes the transition; a second on the same cut is refused (captures in $work)" -ForegroundColor Green
exit 0
