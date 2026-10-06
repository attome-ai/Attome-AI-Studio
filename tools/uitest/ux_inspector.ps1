# UI test: the Inspector's repairs (UX review B3, B4, B8).
#  - a prompt is word-wrapped and its box grows with the text (it was one clipped line in a box two lines tall)
#  - the Variables chips stay inside their panel (they ran off the right edge)
#  - a speech model with ready-made voices shows the Voice input as a list, and a choice is saved in the clip
# Mock engine, virtual input only. Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_inspector.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Inspector.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null
    $id = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data.id
    @"
{"ops":[
 {"op":"add","path":"$id/variables/`$new:a","value":{"name":"style","type":"text","value":"warm golden colours"}},
 {"op":"add","path":"$id/variables/`$new:b","value":{"name":"character","type":"text","value":"a man in a cream sweater"}},
 {"op":"add","path":"$id/variables/`$new:c","value":{"name":"girl","type":"text","value":"a woman with long dark hair"}}
]}
"@ | Set-Content "$work\vars.json" -Encoding utf8
    & "$bin\attome.exe" patch $proj "$work\vars.json" | Out-Null
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $longPrompt = 'A cozy bedroom in the early morning. {character} wakes up and reaches out his hand to the empty pillow beside him, pale sunlight on the bed. Slow zoom-in. {style}'
  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
    'click @rail:Generate', 'wait 400'
    'click @model:attome-mock', 'wait 700'
    'expect @field:prompt'
    'click @field:prompt'
    "type $longPrompt"
    'click @rail:Generate', 'wait 500'
    'high @field:prompt 90'                        # the box has grown to show the whole prompt
    'inside @button:variable_chip_style'
    'inside @button:variable_chip_character'
    'inside @button:variable_chip_girl'              # the last chip is the one that used to leave the panel
    "shot $work\inspector_prompt.jpg"
    'click @rail:Generate', 'click @tab:Audio', 'wait 300'
    'click @model:attome-mock-choir', 'wait 800'
    'expect @combo:input_voice'                      # ready-made voices: a list
    'click @combo:input_voice', 'wait 300'
    "shot $work\inspector_voice_list.jpg"
    'click @option:input_voice_cy', 'wait 700'
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $tracks = Get-Tracks $run
      $voice = $tracks | Where-Object { $_.kind -eq 'audio' } | Select-Object -First 1
      $clip = Get-Object $run $voice.clip_list[0].id
      "voice clip '$($clip.name)': voice = $($clip.media_ref.inputs.voice)"
      if ($clip.media_ref.inputs.voice -ne 'cy') { $failed = "the voice chosen in the list was not saved (it is '$($clip.media_ref.inputs.voice)')" }
      else {
        $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
        if (-not $valid.result.ok) { $failed = 'the project is not valid' }
      }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the prompt wraps and grows, the variable chips stay in their panel, a voice is chosen from a list (captures in $work)" -ForegroundColor Green
exit 0
