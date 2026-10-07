# UI test: a video with sound imports as ONE clip on one video track (its sound inside it, no audio track); Detach audio makes the sound a clip of its own
# on an audio track, not linked, so moving the picture leaves it; linked again (as an agent would) the two are deleted together.
# Virtual input only (uitest.psm1). Exit code 0 = pass.
#   .\tools\uitest\linked.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Linked.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 4 --height 540"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Get-Pair($run) {
  $tracks = @(Get-Tracks $run)
  $v = ($tracks | Where-Object { $_.kind -eq 'video' }).clip_list
  $a = ($tracks | Where-Object { $_.kind -eq 'audio' }).clip_list
  [pscustomobject]@{ Tracks = $tracks.Count; Picture = $(if ($v) { Get-Object $run $v[0].id }); PictureId = $(if ($v) { $v[0].id }); Sound = $(if ($a) { Get-Object $run $a[0].id }); SoundId = $(if ($a) { $a[0].id }) }
}

$run = Invoke-EditorScript -Project $proj -Import "$work\a.mp4" -Script @('wait 500')
$failed = $run.Errors
try {
  if (-not $failed) {
    $p = Get-Pair $run
    "imported: $($p.Tracks) track(s); picture $($p.PictureId) stream '$($p.Picture.media_ref.stream)', sound clip: $($null -ne $p.Sound)"
    if ($p.Tracks -ne 1 -or $p.Sound) { $failed = 'the import should be one clip on one video track, with its sound inside it' }
    elseif ($p.Picture.media_ref.stream) { $failed = "the clip says its stream is '$($p.Picture.media_ref.stream)': it should carry its sound" }
  }
  if (-not $failed) { # Detach audio from the clip's menu: the sound is a clip on an audio track
    $dt = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @("rclick @clip:$($p.PictureId)", 'wait 400', 'expect @menuitem:Detach_audio', 'click @menuitem:Detach_audio', 'wait 900', "shot $work\linked_detached.jpg")
    $failed = $dt.Errors
    $p = Get-Pair $run
    "after Detach audio: $($p.Tracks) tracks; picture stream '$($p.Picture.media_ref.stream)', sound stream '$($p.Sound.media_ref.stream)'"
    if (-not $failed -and (-not $p.Sound -or $p.Picture.media_ref.stream -ne 'video' -or $p.Sound.media_ref.stream -ne 'audio')) { $failed = 'Detach audio did not make a sound clip and a silent picture' }
    if (-not $failed -and ($p.Picture.link_group -or $p.Sound.link_group)) { $failed = 'the detached sound is still linked to its picture' }
  }
  if (-not $failed) { # drag the picture a quarter of its length (1 s) later: the sound stays
    $drag = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @("drag @clip:$($p.PictureId) 25% 0", "shot $work\linked_drag.jpg")
    $failed = $drag.Errors
    $p = Get-Pair $run
    "after the drag: picture at $($p.Picture.timing.record_in), sound at $($p.Sound.timing.record_in)"
    if (-not $failed -and ((ConvertFrom-Rational $p.Picture.timing.record_in) -lt 0.8 -or $p.Sound.timing.record_in -ne '0')) { $failed = 'the detached sound moved with its picture' }
  }
  if (-not $failed) { # link them again (as an agent would): then Delete removes both
    Invoke-Attome $run --json timeline $proj (New-Item -Force "$work\link.json" -Value (@{ ops = @(@{ op = 'link'; clips = @($p.PictureId, $p.SoundId) }) } | ConvertTo-Json -Depth 5)).FullName | Out-Null
    $del = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @("click @clip:$($p.PictureId)", 'key Delete')
    $failed = $del.Errors
    $p = Get-Pair $run
    if (-not $failed -and ($p.Picture -or $p.Sound)) { $failed = 'Delete did not remove the picture and its sound together' }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a video imports as one clip; Detach audio makes two; linked again they are deleted together (captures in $work)" -ForegroundColor Green
exit 0
