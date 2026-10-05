# UI test: a project with nothing in it. The timeline shows the two tracks the first clips will make, dimmed, and a card
# dragged onto them makes the real track and the clip. Mock engine, virtual input only.
#   .\tools\uitest\empty_project.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Empty.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try { & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  # The tiles sit in the left panel and the first lane about 550 points below them; the offsets are from the tile.
  $run = Invoke-EditorScript -Project $proj -Script @(
    'wait 300'
    "shot $work\empty_start.jpg"            # the dimmed V1 and A1 lanes and the hint
    'click @rail:Text'
    'drag @style:Title 300 550 hold'
    'wait 200'
    "shot $work\empty_title_held.jpg"       # the ghost on the lane that will become a track
    'release'
    'wait 600'
    'click @rail:Generate'
    'drag @model:attome-mock 700 550'       # further right on the same lane, now a real track
    'wait 800'
    "shot $work\empty_after.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $tracks = @(Get-Tracks $run)
      $clips = @($tracks | ForEach-Object { $_.clip_list })
      "tracks: $(($tracks | ForEach-Object { "$($_.name) ($(@($_.clip_list).Count))" }) -join ', ')   clips: $(($clips | ForEach-Object { $_.name }) -join ', ')"
      if ($tracks.Count -lt 1) { $failed = 'no track was made' }
      elseif ($clips.Count -ne 2) { $failed = "expected a title and a shot, found $($clips.Count) clips" }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: an empty project shows its lanes, and cards dropped on them make the track and the clips (captures in $work)" -ForegroundColor Green
exit 0
