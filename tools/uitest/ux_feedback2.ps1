# UI test: the second round of the owner's notes.
#  1. a video and its sound that are not linked: the card says so and links them; Speed changes both
#  2. a click on empty space in the Media panel selects nothing; Backspace deletes like Delete
#  3. several clips selected: the values they share; "mixed" where they differ; a change sets all of them
#  4. a text typed on the picture: the picture shows the words as they are typed; Shift+Enter breaks the line, Enter keeps it
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_feedback2.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Notes.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\v.mp4" "--seconds 4 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(
        @{ op = 'add_clip'; separate_audio = $true; id = '$new:v'; path = "$work\v.mp4"; at = '0s' },
        @{ op = 'unlink'; clip = '$new:v' },
        @{ op = 'add_text'; id = '$new:a'; text = 'One'; name = 'One'; at = '0s'; duration = '2s' },
        @{ op = 'add_text'; id = '$new:b'; text = 'Two'; name = 'Two'; at = '2s'; duration = '2s'; track = '$new:a.track' },
        @{ op = 'set_property'; target = '$new:b'; path = 'transform.scale'; value = @(1.5, 1.5) }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message) $($r.error.hint)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Named($run, [string]$name, [string]$kind) {
  foreach ($t in @(Get-Tracks $run)) { if ($kind -and $t.kind -ne $kind) { continue }; foreach ($c in @($t.clip_list)) { $o = Get-Object $run $c.id; if ($o.name -eq $name) { return $o } } }
}
function Count($run) { $n = 0; foreach ($t in @(Get-Tracks $run)) { $n += @($t.clip_list).Count }; $n }

$failed = $null
# 1. the picture of the video, not linked to its sound
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 300 -Script @(
  'wait 1200'
  'click @clip:v', 'wait 500'           # the sound's clip is drawn last: its name's mark is the sound's
  'expect @button:link_again'
  "shot $work\notes_not_linked.jpg"
  'click @button:speed_2', 'wait 900'
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $p = Named $run 'v' 'video'; $s = Named $run 'v' 'audio'
    "speed: picture $($p.timing.speed), sound $($s.timing.speed)"
    if ($p.timing.speed -ne 2 -or $s.timing.speed -ne 2) { $failed = 'Speed did not change both the picture and its unlinked sound' }
  }
  if (-not $failed) {
    $ln = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('click @clip:v', 'wait 400', 'click @button:link_again', 'wait 800', 'click @clip:v', 'wait 400', 'expect @button:unlink', 'absent @button:link_again')
    $failed = $ln.Errors
    if (-not $failed -and (-not (Named $run 'v' 'video').link_group -or (Named $run 'v' 'video').link_group -ne (Named $run 'v' 'audio').link_group)) { $failed = 'Link did not put the picture and its sound in one group' }
  }
  # 2. an empty click in the Media panel; Backspace
  if (-not $failed) {
    $de = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'click @clip:One', 'wait 400', 'expect @field:text_content'
      'click @media:v.mp4@0.5,4', 'wait 400'           # the panel's empty space, below the card
      'absent @field:text_content', 'expect @button:shape_9x16'
      'click @clip:One', 'wait 400', 'key Backspace', 'wait 800', 'absent @clip:One'
      'key Z ctrl', 'wait 800', 'expect @clip:One')
    $failed = $de.Errors
  }
  # 3. both titles: their scales differ (1 and 1.5): "mixed"; 200 % typed sets both
  if (-not $failed) {
    $mu = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'click @clip:One', 'wait 300', 'click @clip:Two ctrl', 'wait 500'
      'expect @mixed:scale', 'expect @slider:multi_rotation'
      "shot $work\notes_multi.jpg"
      'click @number:multi_scale', 'wait 300', 'type 200', 'key Enter', 'wait 900'
      'absent @mixed:scale')
    $failed = $mu.Errors
    if (-not $failed) {
      $a = (Named $run 'One').transform.scale; $b = (Named $run 'Two').transform.scale
      "scales after 200 % on both: $($a -join 'x'), $($b -join 'x')"
      if ($a[0] -ne 2 -or $b[0] -ne 2) { $failed = 'the shared Scale did not set both clips to 200 %' }
    }
  }
  # 4. typed on the picture: the picture follows; Shift+Enter is a new line, Enter keeps it
  if (-not $failed) {
    $tx = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'key Escape', 'wait 300', 'key Home', 'wait 300'
      'click @clip:One', 'wait 300', 'dblclick @monitor', 'wait 500', 'expect @field:monitor_text'
      'key A ctrl', 'type Hello', 'key Enter shift', 'type there', 'wait 600'
      "shot $work\notes_typing.jpg"
      'key Enter', 'wait 900', 'absent @field:monitor_text')
    $failed = $tx.Errors
    if (-not $failed) {
      $said = (Named $run 'One').content.text
      "the text: '$($said -replace "`n", '\n')'"
      if ($said -ne "Hello`nthere") { $failed = "the text is '$said', not two lines 'Hello' and 'there'" }
    }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result | ConvertTo-Json -Compress -Depth 4)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: unlinked picture and sound are said and linked, Speed changes both; an empty click selects nothing; shared values with mixed; text typed on the picture (captures in $work)" -ForegroundColor Green
exit 0
