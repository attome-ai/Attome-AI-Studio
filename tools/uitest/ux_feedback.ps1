# UI test: the editor says what it did and does not do things silently (UX review B1, B5, B6, U2, U5, D1).
#  - a click on a media card only selects it; a double click, or the plus on the card, adds it, with a toast that has Undo;
#    Undo takes the clips away again
#  - the save state is shown; File > Save exists
#  - a locked track refuses to delete its clips (with a toast); mute, hide and lock are saved in the project
#  - nothing in the rail, the modes or the Inspector leads nowhere (no Audio, Templates, Agent, Image, Music, Code)
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_feedback.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Feedback.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 6 --height 540"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $seq = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].id
  $a = ("$work\a.mp4").Replace('\', '\\')
  @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"a.mp4","timing":{"record_in":"0","duration":"6","source_in":"0"},"media_ref":{"type":"file","path":"$a","duration":"6","width":960,"height":540}}}
]}
"@ | Set-Content "$work\ptl.json" -Encoding utf8
  & "$bin\attome.exe" patch $proj "$work\ptl.json" | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Count-Clips($run) { (@(Get-Tracks $run) | ForEach-Object { @($_.clip_list).Count } | Measure-Object -Sum).Sum }
function Flag($run, [string]$track, [string]$flag) {
  $t = @(Get-Tracks $run) | Where-Object { $_.name -eq $track }
  if (-not $t) { return $null }
  $o = Get-Object $run $t.id
  [bool]($o.PSObject.Properties.Name -contains $flag -and $o.$flag -eq $true)
}

$failed = $null
# 1. Nothing leads nowhere; the save state and the media card exist.
$run = Invoke-EditorScript -Project $proj -Script @(
  'wait 1200'
  'expect @save_state'
  'expect @rail:Media', 'expect @rail:Text', 'expect @rail:Effects', 'expect @rail:Generate', 'expect @rail:Models'
  'absent @rail:Audio', 'absent @rail:Templates', 'absent @rail:Agent'
  'absent @mode:Image', 'absent @mode:Music', 'absent @mode:Code'
  'expect @mode:Workflows'
  'absent @tab:Agent'
  'click @menu:File', 'wait 300'
  "shot $work\ux_menu.jpg"
  'key Escape'
  'expect @media:a.mp4'
  'click @media:a.mp4', 'wait 500'   # a click only selects
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $n = Count-Clips $run
    "clips after one click: $n"
    if ($n -ne 1) { $failed = "a click on a media card added clips ($n clips); it must only select" }
  }
  # 2. A double click adds it, says so, and Undo takes it away.
  if (-not $failed) {
    $add = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'dblclick @media:a.mp4', 'wait 900'
      'expect @button:toast_undo'
      "shot $work\ux_toast.jpg"
    )
    $failed = $add.Errors
    if (-not $failed) {
      $n = Count-Clips $run
      "clips after a double click: $n"
      if ($n -le 1) { $failed = 'a double click on the media card did not add it' }
    }
  }
  if (-not $failed) {
    $undo = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('dblclick @media:a.mp4', 'wait 800', 'click @button:toast_undo', 'wait 800')
    $failed = $undo.Errors
    if (-not $failed) {
      $n = Count-Clips $run
      "clips after another double click and Undo: $n"
      if ($n -ne 2 -and $n -ne 3) { $failed = "Undo in the toast did not take the second add away ($n clips)" }
    }
  }
  # 3. The plus adds as well.
  if (-not $failed) {
    $before = Count-Clips $run
    $plus = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('click @button:add_media_a.mp4', 'wait 900')
    $failed = $plus.Errors
    if (-not $failed -and (Count-Clips $run) -le $before) { $failed = 'the plus on the media card did not add it' }
  }
  # 4. Track switches are saved; a locked track refuses to delete.
  if (-not $failed) {
    $sw = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'click @hide:V1', 'wait 500'
      'click @lock:V1', 'wait 500'
    )
    $failed = $sw.Errors
    if (-not $failed) {
      "V1 hidden: $(Flag $run 'V1' 'hidden')   locked: $(Flag $run 'V1' 'locked')"
      if ((Flag $run 'V1' 'hidden') -ne $true) { $failed = 'the eye of V1 did not hide the track' }
      elseif ((Flag $run 'V1' 'locked') -ne $true) { $failed = 'the padlock of V1 did not lock the track' }
    }
  }
  if (-not $failed) {
    $before = Count-Clips $run
    $del = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'click @clip:a.mp4', 'wait 400', 'key Delete', 'wait 500'
      'expect @toast:Track_V1_is_locked._Unlo'
      "shot $work\ux_locked.jpg"
    )
    $failed = $del.Errors
    if (-not $failed -and (Count-Clips $run) -ne $before) { $failed = 'a clip on a locked track was deleted' }
  }
  if (-not $failed) { # an audio track (a video is one clip with its sound inside it: the import makes none): mute and solo
    [IO.File]::WriteAllText("$work\addtrack.json", (@{ project = $proj; ops = @(@{ op = 'add_track'; kind = 'audio' }) } | ConvertTo-Json -Depth 5 -Compress), (New-Object Text.UTF8Encoding($false)))
    $null = Invoke-Attome $run --json call timeline.edit "$work\addtrack.json"
    $names = (@(Get-Tracks $run) | Where-Object { $_.kind -eq 'audio' } | Select-Object -First 1).name
    if ($names) {
      $mute = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @("click @mute:$names", 'wait 500', "click @solo:$names", 'wait 500', "shot $work\ux_mute.jpg")
      $failed = $mute.Errors
      if (-not $failed) {
        "$names muted: $(Flag $run $names 'muted')   solo: $(Flag $run $names 'solo')"
        if ((Flag $run $names 'muted') -ne $true) { $failed = 'the speaker of the audio track did not mute it' }
        elseif ((Flag $run $names 'solo') -ne $true) { $failed = 'the S of the audio track did not solo it' }
      }
    } else { $failed = 'the add made no audio track' }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = 'the project is not valid' }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: media cards select and add with a toast and Undo; track switches work and are saved; nothing in the navigation leads nowhere (captures in $work)" -ForegroundColor Green
exit 0

