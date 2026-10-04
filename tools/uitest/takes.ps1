# UI test: the Takes row of a generative clip's card. Two clips, the second starting from the first. "New take" on the
# first makes a second Take and leaves the second clip out of date; a click on Take 1 brings the first Take back, and
# the second clip is in step again; "Lock this take" pins it. Mock engine: no model, no GPU.
# Virtual input only (uitest.psm1). Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\takes.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Takes.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try { & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  # The two clips come from the Generate panel, like a user would make them; then the Takes row is used.
  $run = Invoke-EditorScript -Project $proj -Script @(
    'click @rail:Generate'
    'wait 400'
    'click @field:gen_prompt'
    'type A robot walks'
    'click @button:gen_add'
    'wait 400'
    'click @field:gen_prompt'
    'type It starts to rain'
    'click @check:gen_chain'
    'click @button:gen_add_run'
    'wait 300'
    'expect @button:gen_run'
    'wait 1200'
    'click @clip:Shot_1'
    'wait 400'
    'expect @button:take_1'
    'click @button:gen_run'              # "New take" on the first clip
    'wait 300'
    'expect @button:take_2'
    'wait 1200'
    "shot $work\takes_two.jpg"           # Take 2 plays; the second clip has the amber bar
    'click @button:take_1'
    'wait 800'
    'click @check:gen_lock'
    'wait 800'
    "shot $work\takes_locked.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $clips = @((Invoke-Attome $run --json gen status $proj | ConvertFrom-Json).result.clips)
      "clips: $(($clips | ForEach-Object { "$($_.name) $($_.state) takes=$($_.takes)" }) -join '; ')"
      $first = (Get-Object $run $clips[0].clip).media_ref
      "first clip: selected is take $([array]::IndexOf($first.take_order, $first.selected) + 1) of $(@($first.take_order).Count), locked=$($first.locked)"
      if (@($first.take_order).Count -ne 2) { $failed = 'the first clip does not have two Takes' }
      elseif ($first.selected -ne $first.take_order[0]) { $failed = 'Take 1 is not the selected one' }
      elseif (-not $first.locked -or $clips[0].state -ne 'locked') { $failed = 'the first clip is not locked' }
      elseif ($clips[1].state -ne 'clean') { $failed = 'the second clip is not back in step with Take 1 of the first' }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a new Take, back to the first by a click, and locked (captures in $work)" -ForegroundColor Green
exit 0
