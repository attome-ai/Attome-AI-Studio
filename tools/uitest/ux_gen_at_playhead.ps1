# UI test: a generated clip is added at the playhead (UX review 5, V8), as text and effects are. A 4 s video on V1 and the playhead
# at 2 s: a click on a model's card adds its clip at 2 s on a new track over the video (V1 is taken there), and the playhead goes to
# the clip's end, so a second click adds the next clip there (on V1, free by then). Mock engine. Virtual input only (uitest.psm1).
# Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_gen_at_playhead.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'AtPlayhead.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    New-Sample "$work\v.mp4" "--seconds 4 --height 360"
    & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
    [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\v.mp4"; name = 'video'; at = '0s'; with_audio = $false }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
    $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
    if (-not $r.ok) { throw "setup: $($r.error.message)" }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  # The picture tracks from the bottom up, as "name:clip@start" in seconds.
  function Layers($run) {
    (@(Get-Tracks $run) | Where-Object { $_.kind -ne 'audio' } | ForEach-Object {
        "$($_.name):" + (@($_.clip_list | Sort-Object { ConvertFrom-Rational $_.record_in } | ForEach-Object { '{0}@{1:0.#}' -f $_.name, (ConvertFrom-Rational $_.record_in) }) -join ',') }) -join ' '
  }

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script @(
    'wait 1200'
    'key Home', 'key Right shift', 'key Right shift', 'wait 300'   # the playhead at 2 s, inside the video
    'click @rail:Generate', 'wait 500'
    'click @model:attome-mock', 'wait 900'                         # over the video, at 2 s
    'click @rail:Generate', 'wait 300'                             # out of the prompt field
    'click @model:attome-mock', 'wait 900'                         # where the first one ends
    "shot $work\two_clips.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $l = Layers $run
      "layers: $l"
      if ($l -ne 'V1:video@0,Shot 2@7 V2:Shot 1@2') { $failed = "the clips are at '$l', not Shot 1 over the video at 2 s and Shot 2 after it at 7 s" }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a generated clip goes at the playhead, over the video when it is there, and the next one follows it (captures in $work)" -ForegroundColor Green
exit 0
