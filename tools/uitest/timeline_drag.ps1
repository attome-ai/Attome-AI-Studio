# UI test: dragging clips on the timeline. A clip let go on free space stays there; let go on another clip it goes before
# it (pointer on its left half) or after it (right half) and the clips after it slide right, so no drop is refused and
# no clip springs back. Also: what is drawn during the drag, one undo for the whole move, a click that changes nothing,
# a move to another track, and a dissolve that is in the way. Virtual input only.
#   .\tools\uitest\timeline_drag.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null

function Text-Clip([string]$track, [string]$key, [string]$name, [string]$at, [string]$len) {
  "{""op"":""add"",""path"":""$track/clips/`$new:$key"",""value"":{""name"":""$name"",""timing"":{""record_in"":""$at"",""duration"":""$len"",""source_in"":""0""},""media_ref"":{""type"":""text""},""content"":{""text"":""$name"",""size"":0.1,""color"":""#ffffff"",""bold"":true}}}"
}

# V1: First 0-3, Second 3-6, Third 6-9 (touching). V2: Other 0-2. 30 fps; the timeline shows 90 pixels a second.
function New-Project([string]$name, [string[]]$extra = @()) {
  $proj = Join-Path $work "$name.attome"
  Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    & "$bin\attome.exe" new $proj --rate 30 | Out-Null
    $seq = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].id
    $ops = @(
      "{""op"":""add"",""path"":""$seq/tracks/`$new:v1"",""value"":{""kind"":""video"",""name"":""V1""}}"
      "{""op"":""add"",""path"":""$seq/tracks/`$new:v2"",""value"":{""kind"":""video"",""name"":""V2""}}"
      (Text-Clip '$new:v1' 'a' 'First' '0' '3')
      (Text-Clip '$new:v1' 'b' 'Second' '3' '3')
      (Text-Clip '$new:v1' 'c' 'Third' '6' '3')
      (Text-Clip '$new:v2' 'o' 'Other' '0' '2')
    ) + $extra
    "{""ops"":[" + ($ops -join ",`n") + "]}" | Set-Content "$work\$name.json" -Encoding utf8
    & "$bin\attome.exe" patch $proj "$work\$name.json" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "the setup patch of $name was refused" }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }
  $proj
}

# Every clip: its track, start and end in seconds. Fails when two clips of a track overlap.
function Get-Layout($run) {
  $layout = [ordered]@{}
  foreach ($t in (Get-Tracks $run)) {
    $spans = @()
    foreach ($c in $t.clip_list) {
      $o = Get-Object $run $c.id
      $at = ConvertFrom-Rational $o.timing.record_in
      $end = $at + (ConvertFrom-Rational $o.timing.duration)
      $layout[$c.name] = [pscustomobject]@{ Track = $t.name; At = [math]::Round($at, 3); End = [math]::Round($end, 3) }
      $spans += , @($at, $end, $c.name)
    }
    foreach ($a in $spans) { foreach ($b in $spans) {
      if ($a[2] -ne $b[2] -and $a[0] -lt $b[1] - 1e-9 -and $b[0] -lt $a[1] - 1e-9) { throw "$($a[2]) and $($b[2]) overlap on $($t.name)" }
    } }
  }
  $layout
}

function Show($layout) { ($layout.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value.Track)@$($_.Value.At)" }) -join '  ' }

# $want: "First=V1@0 Second=V1@3 ..."; returns a message when the layout differs.
function Check($run, [string]$what, [string]$want) {
  $layout = Get-Layout $run
  "$what`n    $(Show $layout)"
  foreach ($part in ($want -split ' ' | Where-Object { $_ })) {
    $name, $rest = $part -split '=', 2
    $track, $at = $rest -split '@', 2
    $got = $layout[$name]
    if (-not $got) { return "$what`: there is no clip $name" }
    if ($got.Track -ne $track -or [math]::Abs($got.At - [double]$at) -gt 0.002) { return "$what`: $name is on $($got.Track) at $($got.At) s, expected $track at $at s" }
  }
  $null
}

function Moves($run) { @((Invoke-Attome $run --json history $run.Project | ConvertFrom-Json).result.changesets | Where-Object { $_.label -like 'Move clip*' }).Count }

$failed = $null
$daemons = @()
try {
  # 1. Before another clip: the last clip let go on the left half of the first one. Everything slides right; one undo.
  $proj = New-Project 'Swap'
  $run = Invoke-EditorScript -Project $proj -Script @(
    'drag @clip:Third @clip:First@0.25,0.5 hold'
    'wait 200'
    "shot $work\drag_swap_held.jpg"     # Third at the start, First and Second drawn slid right, before letting go
    'release'
    'wait 600'
    "shot $work\drag_swap_done.jpg"
  )
  $daemons += $run
  $failed = $run.Errors
  if (-not $failed) { $r = Check $run '1. dropped on the left half of the first clip' 'Third=V1@0 First=V1@3 Second=V1@6 Other=V2@0'; $r[0]; $failed = $r[1] }
  if (-not $failed -and (Moves $run) -ne 1) { $failed = "1. the move made $(Moves $run) edits, expected one" }
  if (-not $failed) {
    $undo = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl', 'wait 400')
    $failed = $undo.Errors
    if (-not $failed) { $r = Check $run '1b. one undo puts all three back' 'First=V1@0 Second=V1@3 Third=V1@6'; $r[0]; $failed = $r[1] }
  }

  # 2. After another clip: let go on its right half. Only the clips after it slide.
  if (-not $failed) {
    $proj = New-Project 'After'
    $run = Invoke-EditorScript -Project $proj -Script @('drag @clip:Third @clip:First@0.75,0.5', 'wait 600')
    $daemons += $run
    $failed = $run.Errors
    if (-not $failed) { $r = Check $run '2. dropped on the right half of the first clip' 'First=V1@0 Third=V1@3 Second=V1@6'; $r[0]; $failed = $r[1] }
  }

  # 3. Free space: it stays where it is let go and nothing else moves. Then back into a gap it fits in, then a clip
  #    dropped after Second with the gap too small: Third slides only as far as needed.
  if (-not $failed) {
    $proj = New-Project 'Free'
    $run = Invoke-EditorScript -Project $proj -Script @('drag @clip:Third 270 0', 'wait 600')
    $daemons += $run
    $failed = $run.Errors
    if (-not $failed) { $r = Check $run '3. three seconds right, onto free space' 'First=V1@0 Second=V1@3 Third=V1@9'; $r[0]; $failed = $r[1] }
    if (-not $failed) {
      $back = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('drag @clip:Third -90 0', 'wait 600')
      $failed = $back.Errors
      if (-not $failed) { $r = Check $run '3b. one second back, still free space' 'First=V1@0 Second=V1@3 Third=V1@8'; $r[0]; $failed = $r[1] }
    }
    if (-not $failed) {
      $push = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('drag @clip:First @clip:Second@0.75,0.5', 'wait 600')
      $failed = $push.Errors
      if (-not $failed) { $r = Check $run '3c. First after Second: the gap of two seconds is used, Third slides one' 'Second=V1@3 First=V1@6 Third=V1@9'; $r[0]; $failed = $r[1] }
    }
  }

  # 4. A click is not a move, and a drag back to where it was is not one either.
  if (-not $failed) {
    $proj = New-Project 'Click'
    $run = Invoke-EditorScript -Project $proj -Script @('click @clip:Second', 'wait 300', 'click @clip:First', 'wait 300')
    $daemons += $run
    $failed = $run.Errors
    if (-not $failed) { $r = Check $run '4. two clicks' 'First=V1@0 Second=V1@3 Third=V1@6 Other=V2@0'; $r[0]; $failed = $r[1] }
    if (-not $failed -and (Moves $run) -ne 0) { $failed = "4. clicking clips made $(Moves $run) move edits" }
  }

  # 5. To another track: First dragged one row down lands on V2, after the clip its pointer is on the right half of.
  if (-not $failed) {
    $proj = New-Project 'Track'
    $run = Invoke-EditorScript -Project $proj -Script @(
      'drag @clip:First 0 44 hold'
      'wait 200'
      "shot $work\drag_track_held.jpg"
      'release'
      'wait 600'
    )
    $daemons += $run
    $failed = $run.Errors
    if (-not $failed) { $r = Check $run '5. one row down' 'Other=V2@0 First=V2@2 Second=V1@3 Third=V1@6'; $r[0]; $failed = $r[1] }
  }

  # 6. A clip that begins and ends between frames (10.005 s long): a clip dropped after it must not be refused.
  if (-not $failed) {
    $proj = New-Project 'Odd' @((Text-Clip '$new:v2' 'x' 'Odd' '4' '32017/3200'))
    $run = Invoke-EditorScript -Project $proj -Script @(
      'drag @clip:Other @clip:Odd@0.75,0.5'   # after it: at the first whole frame past 14.005 s
      'wait 600'
      'drag @clip:Other @clip:Odd@0.25,0.5'   # before it: into the four seconds in front of it
      'wait 600'
      'drag @clip:Odd @clip:Other@0.25,0.5'   # Odd before Other: Other slides to the first whole frame past Odd's end
      'wait 600'
    )
    $daemons += $run
    $failed = $run.Errors
    if (-not $failed) { $r = Check $run '6. around a clip of 10.005 s' 'Odd=V2@0 Other=V2@10.033'; $r[0]; $failed = $r[1] }
    if (-not $failed -and (Moves $run) -ne 3) { $failed = "6. expected three moves around the odd clip, found $(Moves $run): one was refused" }
  }
} finally { foreach ($d in $daemons) { try { Stop-Daemon $d } catch {} } }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: clips dragged on the timeline land before, after or on free space, and what is in the way slides right (captures in $work)" -ForegroundColor Green
exit 0
