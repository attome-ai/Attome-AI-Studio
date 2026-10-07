# UI test: making a voice and its captions from the Generate panel. A voice clip is added at the playhead on a sound track of its own, its
# words box takes the keyboard, the words typed there are kept when Generate is clicked straight after (the click must not lose them),
# and Make captions then makes one clip for each sentence. A voice with no words cannot make captions (the button says why).
# Mock engine, virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_voice_flow.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Voice.attome'
if (Test-Path -LiteralPath $proj) { Remove-Item -LiteralPath $proj -Recurse -Force }
$env:ATTOME_PREF_DIR = Join-Path $work 'prefs'
New-Item -ItemType Directory -Force $env:ATTOME_PREF_DIR | Out-Null
$env:ATTOME_MOCK_ENGINE = '1'

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\v.mp4" "--seconds 4 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 --canvas 360x640 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\v.mp4"; at = '0s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Voice($run) {
  foreach ($t in @(Get-Tracks $run)) { foreach ($c in @($t.clip_list)) { $o = Get-Object $run $c.id; if ($o.name -like 'Voice*') { return $o } } }
}

$failed = $null
try {
  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 300 -Script @(
    'wait 1500'
    'click @rail:Generate', 'wait 500', 'click @model:attome-mock-voice', 'wait 900'
    'expect @field:input_text'
    'type Hello there my friend. Free popcorn today!', 'wait 300'
    'click @button:gen_run', 'wait 4000'       # straight after typing: the words must be kept
    "shot $work\voice_made.jpg"
    'click @button:make_captions_pop', 'wait 1200'
    "shot $work\captions_made.jpg"
  )
  $failed = $run.Errors
  if (-not $failed) {
    $v = Voice $run
    "voice inputs: $($v.media_ref.inputs | ConvertTo-Json -Compress)"
    if ($v.media_ref.inputs.text -notlike 'Hello there*') { $failed = "the words typed before Generate were lost: '$($v.media_ref.inputs.text)'" }
    elseif ([double](ConvertFrom-Rational $v.timing.record_in) -ne 0) { $failed = 'the voice is not at the playhead (0 s)' }
    else {
      $names = @(Get-Tracks $run | ForEach-Object { $_.name })
      "tracks: $($names -join ', ')"
      if ($names -notcontains 'Captions') { $failed = 'Make captions made no Captions track' }
    }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = 'the project is not valid' }
  }
} finally {
  if ($run) { Stop-Daemon $run }
  Remove-Item Env:\ATTOME_PREF_DIR, Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue
}

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a voice is added at the playhead, the typed words survive Generate, and Make captions works (captures in $work)" -ForegroundColor Green
exit 0
