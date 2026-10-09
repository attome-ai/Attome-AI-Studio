# UI test: a video with its sound is one clip; the library keeps it whole, or only its sound, or only its picture (the owner's note: "add the sound only"
# added two clips). "Add the sound only" makes an item of ONE sound clip, no picture; "Add the video only" one silent picture; "Add both" the clip as it is.
# The test's library is its own folder: the user's is never touched. Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_library_part.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$a = Join-Path $work 'A.attome'
$lib = Join-Path $work 'library'
foreach ($p in $a, $lib) { if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force } }
$env:ATTOME_LIBRARY_DIR = $lib
$env:ATTOME_PREF_DIR = Join-Path $work 'prefs'
New-Item -ItemType Directory -Force $env:ATTOME_PREF_DIR | Out-Null

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\clip.mp4" "--seconds 3 --height 360"
  & "$bin\attome.exe" new $a --rate 30 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $a; ops = @(@{ op = 'add_clip'; path = "$work\clip.mp4"; at = '0s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Items { @(Get-ChildItem (Join-Path $lib 'clips') -Directory -ErrorAction SilentlyContinue | ForEach-Object { Get-Content (Join-Path $_.FullName 'item.json') -Raw | ConvertFrom-Json }) }

$failed = $null
$run = Invoke-EditorScript -Project $a -TimeoutSeconds 300 -Script @(
  'wait 1200'
  'rclick @clip:clip', 'wait 400'
  'click @menuitem:Add_to_the_library', 'wait 300', 'expect @menuitem:Both', 'expect @menuitem:Video_only'
  "shot $work\menu.jpg"
  'click @menuitem:Sound_only', 'wait 900'
  'click @rail:Library', 'wait 700'
  "shot $work\library_sound.jpg"
  'click @rail:Media', 'wait 400'
  'rclick @clip:clip', 'wait 400', 'click @menuitem:Add_to_the_library', 'wait 300', 'click @menuitem:Video_only', 'wait 900'
  'rclick @clip:clip', 'wait 400', 'click @menuitem:Add_to_the_library', 'wait 300', 'click @menuitem:Both', 'wait 900'
)
$failed = $run.Errors
Stop-Daemon $run
if (-not $failed) {
  $items = @(Items)
  $line = ($items | ForEach-Object { "$(@($_.clips).Count) clip, picture=$($_.picture), sound=$($_.sound)" }) -join ' | '
  "items: $($items.Count): $line"
  if ($items.Count -ne 3) { $failed = "the library has $($items.Count) items, not 3" }
  elseif (@($items | Where-Object { @($_.clips).Count -ne 1 }).Count) { $failed = 'an item has more than one clip: a video with its sound is one clip, whatever part is kept' }
  else {
    $soundOnly = @($items | Where-Object { $_.sound -and -not $_.picture })
    $pictureOnly = @($items | Where-Object { $_.picture -and $_.clips[0].clip.media_ref.stream -eq 'video' })
    $whole = @($items | Where-Object { $_.picture -and -not $_.clips[0].clip.media_ref.stream })
    if ($soundOnly.Count -ne 1) { $failed = "expected one sound-only item, found $($soundOnly.Count)" }
    elseif ($pictureOnly.Count -ne 1) { $failed = "expected one video-only (silent) item, found $($pictureOnly.Count)" }
    elseif ($whole.Count -ne 1) { $failed = "expected one whole clip item, found $($whole.Count)" }
  }
}
Remove-Item Env:\ATTOME_LIBRARY_DIR, Env:\ATTOME_PREF_DIR -ErrorAction SilentlyContinue

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the library keeps a video's sound only, its picture only, or the whole clip: one clip each (captures in $work)" -ForegroundColor Green
exit 0
