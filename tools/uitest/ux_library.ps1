# UI test: the clip library (the owner's note: keep a clip from the timeline in a library any project can use).
#  - project A: a video's menu, "Add to the library": the item (its picture and sound, with copies of the file) is in the Library panel
#  - project B (another editor and daemon, the same library): a double click puts it at the playhead, picture and sound
#  - its menu renames it and takes it out of the library
# The test's library is its own folder: the user's is never touched. Virtual input only (uitest.psm1). Exit code 0 = pass.
#   .\tools\uitest\ux_library.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$a = Join-Path $work 'A.attome'; $b = Join-Path $work 'B.attome'
$lib = Join-Path $work 'library'
foreach ($p in $a, $b, $lib) { Remove-Item $p -Recurse -Force -ErrorAction SilentlyContinue }
$env:ATTOME_LIBRARY_DIR = $lib

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\clip.mp4" "--seconds 3 --height 360"
  & "$bin\attome.exe" new $a --rate 30 | Out-Null
  & "$bin\attome.exe" new $b --rate 30 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $a; ops = @(@{ op = 'add_clip'; separate_audio = $true; path = "$work\clip.mp4"; at = '0s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Clips($run) { $n = 0; foreach ($t in @(Get-Tracks $run)) { $n += @($t.clip_list).Count }; $n }

$failed = $null
$runA = Invoke-EditorScript -Project $a -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'rclick @clip:clip', 'wait 400', 'expect @menuitem:Add_both_to_the_library', 'click @menuitem:Add_both_to_the_library', 'wait 900'
  'click @rail:Library', 'wait 700', 'expect @library:clip'
  "shot $work\library_a.jpg"
)
$failed = $runA.Errors
Stop-Daemon $runA
try {
  if (-not $failed) {
    $items = @(Get-ChildItem (Join-Path $lib 'clips') -Directory)
    "library items: $($items.Count); files of the first: $((Get-ChildItem (Join-Path $items[0].FullName 'media')).Name -join ', ')"
    if ($items.Count -ne 1) { $failed = "the library has $($items.Count) items, not 1" }
    elseif (-not (Test-Path (Join-Path $items[0].FullName 'thumb.jpg'))) { $failed = 'the item has no picture' }
    else { Remove-Item "$work\clip.mp4" -Force }   # the item has its own copy: project B does not need the original
  }
  $runB = $null
  if (-not $failed) {
    $runB = Invoke-EditorScript -Project $b -TimeoutSeconds 240 -Script @(
      'wait 1200'
      'click @rail:Library', 'wait 700', 'expect @library:clip'
      'dblclick @library:clip', 'wait 1200'
      'expect @clip:clip'
      "shot $work\library_b.jpg"
      'rclick @library:clip', 'wait 400', 'click @menuitem:Rename', 'wait 400', 'expect @field:library_name', 'key A ctrl', 'type Intro', 'key Enter', 'wait 800'
      'expect @library:Intro'
      'drag @library:Intro @clip:clip@1.6,-0.6', 'wait 1200'  # dragged onto the picture track (the row above the sound's mark), after the clip there
      'drag @library:Intro @clip:clip@0.3,-0.6', 'wait 1200'  # and right on top of a clip: it goes on tracks that are free there, nothing overlaps
      "shot $work\on_top.jpg"
      'rclick @library:Intro', 'wait 400', 'click @menuitem:Remove_from_the_library', 'wait 800'
      'absent @library:Intro'
      'expect @clip:clip'                                   # the project keeps its clips
    )
    $failed = $runB.Errors
    if (-not $failed) {
      $n = Clips $runB
      $kinds = @(Get-Tracks $runB | Where-Object { @($_.clip_list).Count -gt 0 } | ForEach-Object { $_.kind }) -join ','
      "project B: $n clips on tracks of kind $kinds"
      if ($n -ne 6) { $failed = "project B has $n clips, not three times the picture and its sound (a double click and two drags)" }
      elseif ($kinds -notmatch 'video' -or $kinds -notmatch 'audio') { $failed = 'the picture and the sound are not on a picture and a sound track' }
      else {
        $valid = Invoke-Attome $runB --json validate $b | ConvertFrom-Json
        if (-not $valid.result.ok) { $failed = "project B is not valid: $($valid.result | ConvertTo-Json -Compress -Depth 4)" }
      }
    }
    if (-not $failed -and @(Get-ChildItem (Join-Path $lib 'clips') -Directory).Count -ne 0) { $failed = 'the item was not removed from the library' }
  }
} finally { if ($runB) { Stop-Daemon $runB }; Remove-Item Env:\ATTOME_LIBRARY_DIR -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a clip kept from one project goes into another from the Library panel, with its sound and its own file; renamed and removed (captures in $work)" -ForegroundColor Green
exit 0
