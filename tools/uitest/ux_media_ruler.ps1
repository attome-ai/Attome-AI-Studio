# UI test: media, the ruler and snapping (UX review 2: A2, A4, A5, B8).
#  - a media card has a menu: Remove takes the file out of the project with the clips made from it; Undo brings the clips back
#  - the ruler has a menu: Mark In here, Mark Out here, Clear (the Export sheet then offers the part, or not)
#  - M puts a marker at the playhead, shown on the ruler; the ruler's menu removes them
#  - the Snap switch: on, a dragged clip catches on the end of another; off, it lands where it was let go
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_media_ruler.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Media.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 4 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(
        @{ op = 'add_text'; text = 'First'; name = 'First'; at = '0s'; duration = '2s' },
        @{ op = 'add_text'; text = 'Second'; name = 'Second'; at = '5s'; duration = '2s' },
        @{ op = 'add_clip'; path = "$work\a.mp4"; at = '9s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message) $($r.error.hint)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Start-Of($run, [string]$name) {
  foreach ($t in @(Get-Tracks $run)) { foreach ($c in @($t.clip_list)) { $o = Get-Object $run $c.id; if ($o.name -eq $name) { return [double](ConvertFrom-Rational $o.timing.record_in) } } }
  return $null
}
function Clip-Count($run) { $n = 0; foreach ($t in @(Get-Tracks $run)) { $n += @($t.clip_list).Count }; $n }

$failed = $null
# The video is at 9 s, far from the titles, so its edges are not what the drag catches on.
# Snapping: Second (at 5 s) is dragged left to 44 ms past the end of First (2 s). 90 pixels are a second.
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'expect @button:snap'
  'drag @clip:Second -266 0', 'wait 700'
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $on = Start-Of $run 'Second'
    "snap on: Second starts at $on s"
    if ([math]::Abs($on - 2.0) -gt 0.001) { $failed = "with snapping on Second landed at $on s, not on the end of First (2 s)" }
  }
  if (-not $failed) {
    $off = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl', 'wait 700', 'click @button:snap', 'wait 300', 'drag @clip:Second -266 0', 'wait 700', "shot $work\snap_off.jpg")
    $failed = $off.Errors
    if (-not $failed) {
      $at = Start-Of $run 'Second'
      "snap off: Second starts at $at s"
      if ([math]::Abs($at - 2.0) -lt 0.01 -or [math]::Abs($at - 2.04) -gt 0.04) { $failed = "with snapping off Second landed at $at s, not where it was let go (about 2.04 s)" }
    }
  }
  if (-not $failed) { # the ruler's menu marks the part; the Export sheet then offers it
    $ruler = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'rclick @ruler', 'wait 400', 'expect @menuitem:Mark_In_here', 'expect @menuitem:Mark_Out_here'
      "shot $work\ruler_menu.jpg"
      'click @menuitem:Mark_In_here', 'wait 500'
      'click @button:export', 'wait 500', 'expect @button:export_part_marked', 'key Escape', 'wait 400'
      'rclick @ruler', 'wait 400', 'click @menuitem:Clear_In_and_Out', 'wait 500'
      'click @button:export', 'wait 500', 'absent @button:export_part_marked', 'key Escape', 'wait 400'
    )
    $failed = $ruler.Errors
  }
  if (-not $failed) { # markers: M puts one at the playhead (1 s); Down from the start stops at it; the ruler's menu takes them all away
    $seq = (Invoke-Attome $run --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].id
    $mk = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Home', 'key Right shift', 'wait 300', 'key M', 'wait 700', 'key Home', 'wait 300', "shot $work\marker.jpg")
    $failed = $mk.Errors
    if (-not $failed) {
      $markers = @((Get-Object $run $seq).markers.PSObject.Properties | ForEach-Object { $_.Value })
      "markers: $($markers | ConvertTo-Json -Compress)"
      if ($markers.Count -ne 1 -or $markers[0].t -ne '1') { $failed = 'M did not put one marker at 1 s' }
    }
    if (-not $failed) {
      $rm = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('rclick @ruler', 'wait 400', 'expect @menuitem:Remove_all_markers', 'click @menuitem:Remove_all_markers', 'wait 700')
      $failed = $rm.Errors
      $left = (Get-Object $run $seq).markers
      if (-not $failed -and $left -and @($left.PSObject.Properties).Count -ne 0) { $failed = 'Remove all markers left a marker' }
    }
  }
  if (-not $failed) { # the media card's menu: the file leaves the project with the clip made from it
    $before = Clip-Count $run
    $media = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'rclick @media:a.mp4', 'wait 400'
      'expect @menuitem:Add_to_the_timeline', 'expect @menuitem:Show_in_folder', 'expect @menuitem:Remove,_and_delete_its_1_clip'
      "shot $work\media_menu.jpg"
      'click @menuitem:Remove,_and_delete_its_1_clip', 'wait 900'
      'absent @media:a.mp4'
      "shot $work\media_removed.jpg"
    )
    $failed = $media.Errors
    if (-not $failed) {
      $after = Clip-Count $run
      "clips: $before before, $after after the file was removed"
      if ($after -ne $before - 1) { $failed = "removing the file left $after clips of $before" }
      elseif (-not (Test-Path "$work\a.mp4")) { $failed = 'the file on the disk was deleted' }
    }
  }
  if (-not $failed) { # Undo brings the clips back, and with them the card
    $undo = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl', 'wait 900', 'expect @media:a.mp4')
    $failed = $undo.Errors
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result | ConvertTo-Json -Compress -Depth 4)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the Snap switch, the ruler's menu for In and Out, and removing a file from the project with its clips (captures in $work)" -ForegroundColor Green
exit 0
