# UI test: a video with sound imports as linked picture and sound clips; dragging the picture moves its sound, Delete
# removes both, and Unlink separates them. Virtual input only (uitest.psm1). Exit code 0 = pass.
#   .\tools\uitest\linked.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP 'attome-uitest'
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Linked.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" sample "$work\a.mp4" --seconds 4 --height 540 | Out-Null
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Get-Pair($run) {
  $tracks = @(Get-Tracks $run)
  $v = ($tracks | Where-Object { $_.kind -eq 'video' }).clip_list
  $a = ($tracks | Where-Object { $_.kind -eq 'audio' }).clip_list
  [pscustomobject]@{ Picture = $(if ($v) { Get-Object $run $v[0].id }); PictureId = $(if ($v) { $v[0].id }); Sound = $(if ($a) { Get-Object $run $a[0].id }); SoundId = $(if ($a) { $a[0].id }) }
}

$run = Invoke-EditorScript -Project $proj -Import "$work\a.mp4" -Script @('wait 500')
$failed = $run.Errors
try {
  if (-not $failed) {
    $p = Get-Pair $run
    "imported: picture $($p.PictureId) ($($p.Picture.media_ref.stream)), sound $($p.SoundId) ($($p.Sound.media_ref.stream))"
    if (-not $p.Sound -or $p.Picture.link_group -ne $p.Sound.link_group) { $failed = 'the import did not make linked picture and sound clips' }
  }
  if (-not $failed) { # drag the picture a quarter of its length (1 s) later: the sound follows
    $drag = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @("drag @clip:$($p.PictureId) 25% 0", "shot $work\linked_drag.jpg")
    $failed = $drag.Errors
    $p = Get-Pair $run
    "after the drag: picture at $($p.Picture.timing.record_in), sound at $($p.Sound.timing.record_in)"
    if (-not $failed -and ((ConvertFrom-Rational $p.Picture.timing.record_in) -lt 0.8 -or $p.Picture.timing.record_in -ne $p.Sound.timing.record_in)) {
      $failed = 'the sound did not move with its picture'
    }
  }
  if (-not $failed) { # Unlink from the Inspector, then the sound stays put when the picture moves
    $un = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @("click @clip:$($p.PictureId)", 'click @button:unlink')
    $failed = $un.Errors
    $p = Get-Pair $run
    if (-not $failed -and ($p.Picture.link_group -or $p.Sound.link_group)) { $failed = 'Unlink did not separate the clips' }
  }
  if (-not $failed) { # link again (as an agent would), then Delete removes both
    Invoke-Attome $run --json timeline $proj (New-Item -Force "$work\link.json" -Value (@{ ops = @(@{ op = 'link'; clips = @($p.PictureId, $p.SoundId) }) } | ConvertTo-Json -Depth 5)).FullName | Out-Null
    $del = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @("click @clip:$($p.PictureId)", 'key Delete')
    $failed = $del.Errors
    $p = Get-Pair $run
    if (-not $failed -and ($p.Picture -or $p.Sound)) { $failed = 'Delete did not remove the picture and its sound together' }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: linked picture and sound import, move, unlink and delete together (captures in $work)" -ForegroundColor Green
exit 0
