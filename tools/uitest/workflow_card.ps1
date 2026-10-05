# UI test: the Workflow card of a generative clip and the Variables on the Project card. The card lists the Exposed Inputs of the
# clip's own workflow in its order, one row each, with a control that fits the Data Type (text box, slider with the range the
# workflow says, yes or no, a choice, a file). An input the workflow does not use is dimmed. Inputs are added and removed on the
# card and show on the Clip Inputs node of the graph at once. The Project card keeps the Variables. Mock engine, virtual input only.
#   .\tools\uitest\workflow_card.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Card.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# The Shot with more Exposed Inputs than the model gives: ones of every Data Type, none of them used by the workflow.
$instance = Get-ShotInstance 'a' | ConvertFrom-Json
$more = @{
  mood     = @{ type = 'text'; label = 'Mood'; order = 3 }
  strength = @{ type = 'number'; label = 'Strength'; order = 4; range = @{ min = 0; max = 1 } }
  sharp    = @{ type = 'boolean'; label = 'Sharpen'; order = 5 }
  style    = @{ type = 'text'; label = 'Style'; order = 6; range = @{ options = @('anime', 'film', 'noir') }; default = 'film' }
  photo    = @{ type = 'image'; label = 'Photo'; order = 7 }
}
foreach ($k in $more.Keys) { $instance.exposed.inputs | Add-Member -NotePropertyName $k -NotePropertyValue $more[$k] }
$instanceJson = $instance | ConvertTo-Json -Depth 20 -Compress

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null
    $info = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data
    $prj = $info.id; $seq = $info.sequences[0].id
    @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"First","timing":{"record_in":"0","duration":"2","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":$instanceJson,"inputs":{"prompt":"A robot walks","seed":3}}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
    & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'the setup patch was refused' }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  function Get-Clip($run) { Get-Object $run ((Get-Tracks $run)[0].clip_list[0].id) }

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 180 -Script @(
    'click @clip:First'
    'wait 400'
    'expect @field:prompt'                  # the prompt is a row like the others
    'expect @input:mood'
    'expect @slider:input_strength'
    'expect @check:input_sharp'
    'expect @combo:input_style'
    'expect @button:input_choose_photo'
    "shot $work\card_rows.jpg"              # the rows in the workflow's order; the ones it does not use are dimmed
    'click @field:input_mood'
    'type calm'
    'click @field:prompt'                   # leaving the field saves it
    'wait 300'
    'slide @slider:input_strength 0.5'
    'wait 300'
    'click @check:input_sharp'
    'wait 300'
    "shot $work\card_edited.jpg"
    'click @button:input_add'               # a new input: a name and a Data Type
    'expect @field:new_input_name'
    'click @field:new_input_name'
    'type Camera move'
    'click @button:input_type_integer'
    "shot $work\card_adding.jpg"
    'click @button:input_add_ok'
    'wait 400'
    'expect @input:camera_move'
    'click @button:input_remove_photo'      # an input taken away
    'wait 400'
    "shot $work\card_added.jpg"
    'dblclick @clip:First'                  # the graph: the Clip Inputs node lists the same inputs
    'expect @clipin:camera_move'
    'expect @clipin:mood'
    'wait 300'
    "shot $work\card_graph.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $clip = Get-Clip $run
      $face = $clip.media_ref.workflow.exposed.inputs
      $names = @($face.PSObject.Properties | Sort-Object { $_.Value.order } | ForEach-Object { $_.Name })
      "inputs: $($names -join ', ')"
      "values: $(($clip.media_ref.inputs.PSObject.Properties | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ', ')"
      if ($clip.media_ref.inputs.mood -ne 'calm') { $failed = "the mood is '$($clip.media_ref.inputs.mood)', not 'calm'" }
      elseif ([math]::Abs([double]$clip.media_ref.inputs.strength - 0.5) -gt 0.03) { $failed = "the strength is $($clip.media_ref.inputs.strength), not about 0.5" }
      elseif ($clip.media_ref.inputs.sharp -ne $true) { $failed = 'the Sharpen box did not set true' }
      elseif ($clip.media_ref.inputs.prompt -ne 'A robot walks') { $failed = 'the prompt changed' }
      elseif (-not ($names -contains 'camera_move')) { $failed = 'the new input is not on the clip''s workflow' }
      elseif ($face.camera_move.type -ne 'integer' -or $face.camera_move.label -ne 'Camera move') { $failed = 'the new input has the wrong type or label' }
      elseif ($names[-1] -ne 'camera_move') { $failed = 'the new input is not last in the order' }
      elseif ($names -contains 'photo') { $failed = 'the input that was removed is still there' }
      elseif ($face.camera_move.PSObject.Properties.Name -contains 'to') { $failed = 'a new input should feed nothing yet' }
    }
    if (-not $failed) {
      $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
      if (-not $valid.result.ok) { $failed = 'the project is not valid after the edits' }
    }
  } finally { }

  if (-not $failed) { # the Project card: no clip is selected when the editor opens
    $second = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 120 -Script @(
      'expect @button:variable_add'
      'click @button:variable_add'
      'click @field:new_variable_name'
      'type style'
      'click @button:variable_add_ok'
      'wait 400'
      'expect @variable:style'
      'click @field:variable_style'
      'type anime'
      'wait 200'
      "shot $work\card_variable_typed.jpg"
      'key Enter'                           # leaving the field saves it
      'wait 400'
      "shot $work\card_project.jpg"
    )
    $failed = $second.Errors
    if (-not $failed) {
      $vars = (Invoke-Attome $run --json get $proj $prj | ConvertFrom-Json).result.object.variables
      $v = @($vars.PSObject.Properties)
      "variables: $(($v | ForEach-Object { "$($_.Value.name)($($_.Value.type))=$($_.Value.value)" }) -join ', ')"
      if ($v.Count -ne 1) { $failed = "expected one Variable, found $($v.Count)" }
      elseif ($v[0].Value.name -ne 'style' -or $v[0].Value.type -ne 'text') { $failed = 'the Variable has the wrong name or type' }
      elseif ($v[0].Value.value -ne ' anime' -and $v[0].Value.value -ne 'anime') { $failed = "the Variable's value is '$($v[0].Value.value)'" }
    }
  }
  Stop-Daemon $run
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the Workflow card lists the Exposed Inputs by type, adds and removes them, the graph shows the same list, and the Project card keeps Variables (captures in $work)" -ForegroundColor Green
exit 0
