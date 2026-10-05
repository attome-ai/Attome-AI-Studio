# UI test: values and reuse. Variables of the project are written into a text as {name} (a chip adds one; a name that is no Variable's is
# marked), a picture kept as a Variable feeds a clip from its card, a Variable changes once and every clip that reads it is out of date,
# and a clip's input values are kept as a Preset and put on another clip. Mock engine, virtual input only.
#   .\tools\uitest\workflow_values.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Values.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null
    $info = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data
    $prj = $info.id; $seq = $info.sequences[0].id
    # Two clips of the same Clip Workflow. "style" and "hero" are Variables; the second clip's prompt uses {style} from the start.
    # The second clip's workflow also has a picture input, "photo", that feeds the sampler's reference pictures.
    $second = (Get-ShotInstance 'b').Replace('"exposed":{"inputs":{', '"exposed":{"inputs":{"photo":{"type":"image","label":"Photo","order":9,"to":[["$new:smpb","references"]]},')
    @"
{"ops":[
 {"op":"add","path":"$prj/variables/`$new:style","value":{"name":"style","type":"text","value":"anime"}},
 {"op":"add","path":"$prj/variables/`$new:hero","value":{"name":"hero","type":"image"}},
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"First","timing":{"record_in":"0","duration":"2","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":$(Get-ShotInstance 'a'),"inputs":{"prompt":"A robot walks","seed":3}}}},
 {"op":"add","path":"`$new:v1/clips/`$new:b","value":{"name":"Second","timing":{"record_in":"2","duration":"2","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":$second,"inputs":{"prompt":"It rains in the {style} style","seed":5}}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
    & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'the setup patch was refused' }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  function Get-Clip($run, $name) { foreach ($c in (Get-Tracks $run)[0].clip_list) { if ($c.name -eq $name) { return Get-Object $run $c.id } } }
  function Get-States($run) { (@((Invoke-Attome $run --json gen status $proj | ConvertFrom-Json).result.clips) | ForEach-Object { "$($_.name)=$($_.state)" }) -join ' ' }

  # 1. {name}: a chip adds a Variable to the prompt; a name that is none is marked; the Preset is saved from the first clip.
  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
    'click @clip:First'
    'wait 500'
    'expect @button:variable_chip_style'
    'click @button:variable_chip_style'      # "A robot walks {style}"
    'wait 500'
    "shot $work\values_chip.jpg"
    'click @field:prompt'
    'key End'
    'type  {nobody}'                         # a name that is no Variable's
    'wait 300'
    'expect @unknown_variable:nobody'
    "shot $work\values_unknown.jpg"
    'click @clip:Second'                     # on to the other clip: the edit is made on First
    'wait 500'
    'click @clip:First'
    'wait 500'
    'click @button:preset_new'               # First's values, kept as a Preset
    'expect @field:preset_name'
    'click @field:preset_name'
    'type Close-up'
    'click @button:preset_save'
    'wait 500'
    'click @clip:Second'
    'wait 500'
    'expect @combo:preset'
    'click @combo:preset'
    'click @preset_option:Close-up'          # put on the second clip
    'wait 600'
    "shot $work\values_preset.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $first = Get-Clip $run 'First'; $second = Get-Clip $run 'Second'
      $project = (Invoke-Attome $run --json get $proj $prj | ConvertFrom-Json).result.object
      $presets = @($project.presets.PSObject.Properties)
      "First prompt: $($first.media_ref.inputs.prompt)"
      "Second prompt: $($second.media_ref.inputs.prompt)   seed $($second.media_ref.inputs.seed)   presets: $(($presets | ForEach-Object { $_.Value.name }) -join ',')"
      if ($first.media_ref.inputs.prompt -notmatch '\{style\}') { $failed = 'the chip did not add {style} to the prompt' }
      elseif ($first.media_ref.inputs.prompt -notmatch '\{nobody\}') { $failed = 'the text typed was lost when another clip was clicked' }
      elseif ($presets.Count -ne 1 -or $presets[0].Value.name -ne 'Close-up') { $failed = 'the Preset was not saved' }
      elseif ($second.media_ref.inputs.seed -ne 3) { $failed = 'the Preset did not put its seed on the second clip' }
      elseif ($second.media_ref.inputs.prompt -ne $first.media_ref.inputs.prompt) { $failed = 'the Preset did not put its prompt on the second clip' }
    }
  } finally { }

  # 2. One change of the Variable: every clip whose text uses it is out of date.
  if (-not $failed) {
    $go = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 240 -Script @(
      'click @clip:First'
      'wait 400'
      'click @button:gen_run'
      'wait 300'
      'expect @button:gen_run'
      'wait 1500'
    )
    $failed = $go.Errors
    if (-not $failed) {
      $before = Get-States $run
      '{"ops":[{"op":"replace","path":"' + ((Invoke-Attome $run --json get $proj $prj | ConvertFrom-Json).result.object.variables.PSObject.Properties | Where-Object { $_.Value.name -eq 'style' }).Name + '/value","value":"film"}]}' | Set-Content "$work\var.json" -Encoding utf8
      $changed = Invoke-Attome $run --json patch $proj "$work\var.json" | ConvertFrom-Json
      $after = Get-States $run
      "before: $before   after the Variable changed: $after"
      if (-not $changed.ok) { $failed = 'the change of the Variable was refused' }
      elseif ($before -notmatch 'First=clean') { $failed = 'First was not generated' }
      elseif ($after -notmatch 'First=dirty') { $failed = 'the clip whose text uses the Variable is not out of date' }
    }
  }

  # 3. A picture Variable: the second clip reads it from its card; replacing the Variable once updates the clip.
  if (-not $failed) {
    $pic = Join-Path $work 'hero.png'
    [IO.File]::WriteAllBytes($pic, [byte[]](0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A))
    $hero = ((Invoke-Attome $run --json get $proj $prj | ConvertFrom-Json).result.object.variables.PSObject.Properties | Where-Object { $_.Value.name -eq 'hero' }).Name
    ('{"ops":[{"op":"add","path":"' + $hero + '/value","value":"' + ($pic -replace '\\', '/') + '"}]}') | Set-Content "$work\hero.json" -Encoding utf8
    Invoke-Attome $run --json patch $proj "$work\hero.json" | Out-Null
    $pick = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 240 -Script @(
      'click @clip:Second'
      'wait 500'
      'expect @combo:input_variable_photo'
      'click @combo:input_variable_photo'
      'click @variable_option:hero'
      'wait 600'
      "shot $work\values_picture.jpg"
    )
    $failed = $pick.Errors
    if (-not $failed) {
      $clip2 = Get-Clip $run 'Second'
      $nodes = @($clip2.media_ref.workflow.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq 'attome.variable' })
      "Second reads a picture Variable by $($nodes.Count) Variable node(s)"
      if ($nodes.Count -ne 1 -or $nodes[0].Value.variable -ne $hero) { $failed = 'the picture is not read from the Variable' }
      else {
        $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
        if (-not $valid.result.ok) { $failed = 'the project is not valid after the edits' }
      }
    }
  }
  Stop-Daemon $run
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: {variables} in texts, a picture read from a Variable, one change moving the clips that read it, and Presets applied to another clip (captures in $work)" -ForegroundColor Green
exit 0
