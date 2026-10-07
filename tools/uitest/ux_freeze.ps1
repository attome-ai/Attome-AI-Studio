# UI test: freeze frame (UX review 2: B6). With a video selected and the playhead at 1 s, "Freeze frame" in its menu holds that frame
# for 2 s: the picture at 1.5 s and 2.5 s is the frame at 1 s, the clip goes on after it, and its sound waits. Undo takes it back.
# Reverse, from the same menu, plays the clip and its sound backwards.
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_freeze.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Freeze.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\v.mp4" "--seconds 4 --height 360"     # a bar that moves across the picture
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; separate_audio = $true; path = "$work\v.mp4"; at = '0s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

# The pictures at some times, as small grey rows: equal pictures have rows that match.
function Rows($run, [string[]]$times) {
  [IO.File]::WriteAllText("$work\see.json", (@{ project = $proj; times = $times; height = 90 } | ConvertTo-Json -Compress), (New-Object Text.UTF8Encoding($false)))
  $see = (Invoke-Attome $run --json call see.frames "$work\see.json" | ConvertFrom-Json).result
  Add-Type -AssemblyName System.Drawing
  foreach ($img in $see.images) {
    $bmp = [System.Drawing.Bitmap]::FromFile($img.path)
    $row = for ($x = 0; $x -lt $bmp.Width; $x += 2) { $bmp.GetPixel($x, [int]($bmp.Height / 2)).G }
    $bmp.Dispose()
    , @($row)
  }
}

function Get-RowDifference($a, $b) { [double]$d = 0; $n = [math]::Min(@($a).Count, @($b).Count); for ($i = 0; $i -lt $n; $i++) { $d += [math]::Abs([int]$a[$i] - [int]$b[$i]) }; [double]($d / [math]::Max(1, $n)) }

$failed = $null
$run = Invoke-EditorScript -Project $proj -SelectFirstClip -TimeoutSeconds 240 -Script @('wait 1200')
$failed = $run.Errors
try {
  $before = $null
  if (-not $failed) { $before = Rows $run @('1s', '3s') }
  if (-not $failed) {
    $fz = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @(
      'key Home', 'key Right shift', 'wait 300'
      'rclick @clip:v', 'wait 400', 'expect @menuitem:Freeze_frame_(2_s)'
      "shot $work\freeze_menu.jpg"
      'click @menuitem:Freeze_frame_(2_s)', 'wait 1200'
      "shot $work\freeze_done.jpg"
    )
    $failed = $fz.Errors
  }
  if (-not $failed) {
    $names = foreach ($t in @(Get-Tracks $run)) { foreach ($c in @($t.clip_list)) { $o = Get-Object $run $c.id; "$($t.kind):$($o.name)@$($o.timing.record_in)+$($o.timing.duration)" } }
    "clips: $($names -join ', ')"
    $after = Rows $run @('1.5s', '2.5s', '5s')
    $held1 = Get-RowDifference $after[0] $before[0]; $held2 = Get-RowDifference $after[1] $before[0]; $goes_on = Get-RowDifference $after[2] $before[1]; $moved = Get-RowDifference $before[0] $before[1]
    "difference to the frame at 1 s: at 1.5 s $([math]::Round($held1,1)), at 2.5 s $([math]::Round($held2,1)); 5 s against the old 3 s: $([math]::Round($goes_on,1)); the bar moving 1 s to 3 s: $([math]::Round($moved,1))"
    if ($moved -lt 5) { $failed = 'the sample does not move: this test cannot tell frames apart' }
    elseif ($held1 -gt 3 -or $held2 -gt 3) { $failed = 'the picture during the freeze is not the frame at 1 s' }
    elseif ($goes_on -gt 3) { $failed = 'after the freeze the clip does not go on from where it was held' }
    elseif (-not ($names -match 'freeze')) { $failed = 'no still was put in' }
  }
  if (-not $failed) {
    $u = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl', 'wait 900')
    $failed = $u.Errors
    $count = 0; foreach ($t in @(Get-Tracks $run)) { $count += @($t.clip_list).Count }
    if (-not $failed -and $count -ne 2) { $failed = "after Undo there are $count clips, not the picture and its sound" }
  }
  if (-not $failed) { # Reverse from the same menu: the picture and its sound play backwards; the first frame is now the file's last
    $first_before = (Rows $run @('0s', '3.9s'))
    $rv = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('rclick @clip:v', 'wait 400', 'click @menuitem:Reverse', 'wait 1000', "shot $work\reversed.jpg")
    $failed = $rv.Errors
    if (-not $failed) {
      $flags = foreach ($t in @(Get-Tracks $run)) { foreach ($c in @($t.clip_list)) { [bool](Get-Object $run $c.id).timing.reverse } }
      $first_after = (Rows $run @('0s'))[0]
      $d = Get-RowDifference $first_after $first_before[1]
      "reversed: $($flags -join ', '); its first frame against the old last: $([math]::Round($d, 1))"
      if (@($flags | Where-Object { $_ }).Count -ne 2) { $failed = 'Reverse did not turn both the picture and its sound round' }
      elseif ($d -gt 3) { $failed = 'the reversed clip does not start with the frame it ended on' }
    }
    if (-not $failed) {
      $fw = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('rclick @clip:v', 'wait 400', 'expect @menuitem:Play_forwards', 'click @menuitem:Play_forwards', 'wait 800')
      $failed = $fw.Errors
    }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result | ConvertTo-Json -Compress -Depth 4)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Freeze frame from the clip's menu holds the frame, the clip goes on after it, Undo takes it back (captures in $work)" -ForegroundColor Green
exit 0
