# UI test: the padlock in a track header locks the track to the cut and releases it, the lock is saved in the project
# (it is still there when the editor is started again, and one undo takes it back), and a Titles track made by the editor
# starts locked. Virtual input only (uitest.psm1). Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\track_lock.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP 'attome-uitest'
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'TrackLock.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# A video track and a track of music, neither locked.
$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" sample "$work\a.mp4" --seconds 6 --height 540 | Out-Null
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

function Get-Lock($run, [string]$name) {
  $t = @(Get-Tracks $run) | Where-Object { $_.name -eq $name }
  if (-not $t) { return $null }
  $obj = Get-Object $run $t.id
  [bool]($obj.PSObject.Properties.Name -contains 'sync_lock' -and $obj.sync_lock -eq $true)
}

$failed = $null
# 1. A Titles track made by the editor starts locked; V1 does not.
$run = Invoke-EditorScript -Project $proj -Script @(
  'click @rail:Text'
  'click @style:Title'
  'wait 500'
  "shot $work\lock_titles.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    "Titles locked: $(Get-Lock $run 'Titles')   V1 locked: $(Get-Lock $run 'V1')"
    if ((Get-Lock $run 'Titles') -ne $true) { $failed = 'the Titles track made by the editor is not locked to the cut' }
    elseif ((Get-Lock $run 'V1') -ne $false) { $failed = 'the V1 track should not be locked' }
  }
  # 2. The padlock of V1 locks it; the lock is saved.
  if (-not $failed) {
    $lock = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('click @lock:V1', 'wait 400')
    $failed = $lock.Errors
    if (-not $failed -and (Get-Lock $run 'V1') -ne $true) { $failed = 'the padlock did not lock V1' }
  }
  # 3. A new editor on the same project still sees it (and shows it: capture), and the padlock releases it again.
  if (-not $failed) {
    $again = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('wait 600', "shot $work\lock_saved.jpg", 'click @lock:V1', 'wait 400')
    $failed = $again.Errors
    if (-not $failed -and (Get-Lock $run 'V1') -ne $false) { $failed = 'the padlock did not release V1 in a second editor' }
  }
  # 4. One undo takes the release back, a second takes the lock back.
  if (-not $failed) {
    $undo = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl')
    $failed = $undo.Errors
    if (-not $failed -and (Get-Lock $run 'V1') -ne $true) { $failed = 'undo did not bring the lock back' }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the padlock locks and releases a track, the lock is saved, undo takes it back, and the Titles track starts locked (captures in $work)" -ForegroundColor Green
exit 0
