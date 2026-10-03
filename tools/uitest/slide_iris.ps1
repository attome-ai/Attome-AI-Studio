# UI test: the Transition card adds a slide (from the side chosen with the Left/Right/Top/Bottom buttons) and an iris, and
# removes them again. Each check uses its own project and one editor run. Virtual input only (uitest.psm1). Needs a build
# (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\slide_iris.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP 'attome-uitest'
New-Item -ItemType Directory -Force $work | Out-Null

# Two 3-second clips that touch at 3 s, each with media to spare beyond the cut. Returns the project's path.
function New-Project($name) {
  $proj = Join-Path $work "$name.attome"
  Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    & "$bin\attome.exe" sample "$work\a.mp4" --seconds 6 --height 540 | Out-Null
    & "$bin\attome.exe" sample "$work\b.mp4" --seconds 6 --height 540 | Out-Null
    & "$bin\attome.exe" new $proj --rate 30 | Out-Null
    $seq = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].id
    $a = ("$work\a.mp4").Replace('\', '\\')
    $b = ("$work\b.mp4").Replace('\', '\\')
    @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"a.mp4","timing":{"record_in":"0","duration":"3","source_in":"0"},"media_ref":{"type":"file","path":"$a","duration":"6","width":960,"height":540}}},
 {"op":"add","path":"`$new:v1/clips/`$new:b","value":{"name":"b.mp4","timing":{"record_in":"3","duration":"3","source_in":"2"},"media_ref":{"type":"file","path":"$b","duration":"6","width":960,"height":540}}}
]}
"@ | Set-Content "$work\$name.json" -Encoding utf8
    & "$bin\attome.exe" patch $proj "$work\$name.json" | Out-Null
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }
  $proj
}

function Get-Transitions($run) {
  $track = Get-Object $run (@(Get-Tracks $run) | Where-Object { $_.name -eq 'V1' }).id
  if (-not $track.transitions) { return ,@() }
  ,@($track.transitions.PSObject.Properties | ForEach-Object { $_.Value })
}

$failed = $null

# 1. A slide from the bottom.
$proj = New-Project 'Slide'
$run = Invoke-EditorScript -Project $proj -SelectFirstClip -Script @(
  'click @button:wipe_down'
  'click @button:add_slide'
  'wait 800'
  "shot $work\slide_added.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $t = Get-Transitions $run
    "transition: $($t | ConvertTo-Json -Compress)"
    if ($t.Count -ne 1 -or $t[0].type -ne 'attome.slide' -or $t[0].params.direction -ne 'down') { $failed = 'the Transition card did not add a slide from the bottom' }
  }
} finally { Stop-Daemon $run }

# 2. An iris.
if (-not $failed) {
  $proj = New-Project 'Iris'
  $run = Invoke-EditorScript -Project $proj -SelectFirstClip -Script @(
    'click @button:add_iris'
    'wait 800'
    "shot $work\iris_added.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $t = Get-Transitions $run
      "transition: $($t | ConvertTo-Json -Compress)"
      if ($t.Count -ne 1 -or $t[0].type -ne 'attome.iris' -or [math]::Abs($t[0].params.softness - 0.15) -gt 0.001) { $failed = 'the Transition card did not add an iris' }
    }
  } finally { Stop-Daemon $run }
}

# 3. Both are removed by their own Remove buttons (the button only exists while that transition does).
if (-not $failed) {
  $proj = New-Project 'SlideIrisRemove'
  $run = Invoke-EditorScript -Project $proj -SelectFirstClip -Script @(
    'click @button:add_slide'
    'expect @button:remove_slide'
    'click @button:remove_slide'
    'wait 500'
    'click @button:add_iris'
    'expect @button:remove_iris'
    'click @button:remove_iris'
    'wait 500'
  )
  $failed = $run.Errors
  try {
    if (-not $failed -and (Get-Transitions $run).Count -ne 0) { $failed = 'the Remove buttons did not remove the slide and the iris' }
  } finally { Stop-Daemon $run }
}

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a slide from the bottom and an iris added, and both removed, from the Transition card (captures in $work)" -ForegroundColor Green
exit 0
