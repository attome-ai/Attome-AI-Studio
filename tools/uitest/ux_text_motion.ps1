# UI test: how a text comes in and goes (UX review 3, R3). The Text card has an Animation part: In and Out lists (None, Fade, Pop,
# Slide, Typewriter) and, once one is chosen, a slider for its length. They are saved as content.animate_in and content.animate_out,
# {style, duration}; None takes the field away. The captures show the title part way in. Virtual input only (uitest.psm1). Needs a
# build. Exit code 0 = pass.
#   .\tools\uitest\ux_text_motion.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Motion.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_text'; text = 'Hello there'; name = 'Hello'; at = '0s'; duration = '3s'; size = 0.15 }) } | ConvertTo-Json -Depth 5 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Content($run) { $t = (@(Get-Tracks $run) | Where-Object { @($_.clip_list).Count -ge 1 } | Select-Object -First 1); (Get-Object $run $t.clip_list[0].id).content }
function Has($o, [string]$name) { $o.PSObject.Properties.Name -contains $name }

$failed = $null
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'click @clip:Hello', 'wait 500'
  'expect @field:animate_in', 'expect @field:animate_out', 'absent @slider:text_animate_in'
  'click @field:animate_in', 'wait 400'
  'click @animate_in:Pop', 'wait 800'
  'expect @slider:text_animate_in'
  'click @field:animate_out', 'wait 400'
  'click @animate_out:Typewriter', 'wait 800'
  'slide @slider:text_animate_out 0.9', 'wait 700'
  'key Home', 'key Right', 'key Right', 'key Right', 'wait 800'   # three frames in: the title is popping
  "shot $work\pop_in.jpg"
  'key End', 'key Left', 'key Left', 'key Left', 'key Left', 'key Left', 'key Left', 'key Left', 'key Left', 'key Left', 'key Left', 'wait 800'
  "shot $work\typing_out.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $c = Content $run
    "content: in=$($c.animate_in | ConvertTo-Json -Compress) out=$($c.animate_out | ConvertTo-Json -Compress)"
    if ($c.animate_in.style -ne 'pop' -or $c.animate_in.duration -ne '1/2') { $failed = "animate_in is $($c.animate_in | ConvertTo-Json -Compress), not pop for half a second" }
    elseif ($c.animate_out.style -ne 'typewriter') { $failed = "animate_out is $($c.animate_out | ConvertTo-Json -Compress), not typewriter" }
    elseif ((ConvertFrom-Rational $c.animate_out.duration) -lt 1.5) { $failed = "the slider left the out length at $($c.animate_out.duration)" }
  }
  if (-not $failed) { # None takes the field away
    $back = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('click @clip:Hello', 'wait 400', 'click @field:animate_in', 'wait 300', 'click @animate_in:None', 'wait 700', 'absent @slider:text_animate_in')
    $failed = $back.Errors
    if (-not $failed -and (Has (Content $run) 'animate_in')) { $failed = 'None left content.animate_in on the text' }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result.problems | ConvertTo-Json -Compress)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a text's in and out animations are chosen on its card, saved in its content, and taken away again (captures in $work)" -ForegroundColor Green
exit 0
