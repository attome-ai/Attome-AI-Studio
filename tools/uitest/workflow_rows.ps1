# UI test: the Clip Inputs node and the Output node of the Workflow Canvas. A row is selected by a click on its name; the side panel
# renames it, moves an input up or down, makes an output the Primary Output, or removes it. An input added on the canvas (dragged
# from the "new input" port) shows on the Workflow card, and one added on the card shows on the canvas. Mock engine, virtual input only.
#   .\tools\uitest\workflow_rows.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Rows.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null
    $info = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data
    $seq = $info.sequences[0].id
    @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"First","timing":{"record_in":"0","duration":"2","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":$(Get-ShotInstance 'a'),"inputs":{"prompt":"A robot walks","seed":3}}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
    & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'the setup patch was refused' }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  function Get-Clip($run) { Get-Object $run ((Get-Tracks $run)[0].clip_list[0].id) }
  function Get-Order($clip) { (@($clip.media_ref.workflow.exposed.inputs.PSObject.Properties | Sort-Object { [int]$_.Value.order } | ForEach-Object { $_.Name })) -join ',' }

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 180 -Script @(
    'dblclick @clip:First'
    'expect @clipin:seed'
    'click @row:in:seed'                    # the row: its name, its place, remove
    'expect @field:wf_row_name'
    'expect @button:wf_row_up'
    "shot $work\rows_input.jpg"
    'click @button:wf_row_up'               # the order is prompt, start_image, seed: seed goes up one
    'wait 400'
    'click @field:wf_row_name'
    'key End'
    'type s'
    'key Enter'                             # a new name: the clip's value goes with it
    'wait 400'
    "shot $work\rows_renamed.jpg"
    'drag @clipin:+ @port:sample.end_image' # a new input from the graph: dragged to an input of a node
    'wait 500'
    'click @row:out:last_frame'             # an output: it can be made the main one
    'expect @button:wf_row_main'
    "shot $work\rows_output.jpg"
    'click @button:wf_row_main'
    'wait 400'
    'click @row:out:video'
    'wait 200'
    'click @row:out:last_frame'
    'click @button:wf_row_remove'           # an output taken away: the main mark goes to what is left
    'wait 400'
    "shot $work\rows_after.jpg"
    'click @button:wf_back'
    'click @clip:First'
    'wait 400'
    'expect @input:end_image'               # the input added on the canvas is a row of the Workflow card
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $clip = Get-Clip $run
      $face = $clip.media_ref.workflow.exposed
      "inputs: $(Get-Order $clip) ; outputs: $(@($face.outputs.PSObject.Properties.Name) -join "+") ; primary $($face.primary)"
      "values: $(($clip.media_ref.inputs.PSObject.Properties | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ', ')"
      $order = Get-Order $clip
      if (-not ($order -match '^prompt,seeds,start_image|^prompt,seeds')) { $failed = "the order is ${order}: seed did not go up and get its new name" }
      elseif ($clip.media_ref.inputs.seeds -ne 3) { $failed = "the clip's value did not follow the rename (seeds=$($clip.media_ref.inputs.seeds))" }
      elseif ($clip.media_ref.inputs.PSObject.Properties.Name -contains 'seed') { $failed = 'the old name still holds a value' }
      elseif (-not ($face.inputs.PSObject.Properties.Name -contains 'end_image')) { $failed = 'the input dragged from the graph is not on the workflow' }
      elseif ($face.outputs.PSObject.Properties.Name -contains 'last_frame') { $failed = 'the output was not removed' }
      elseif ($face.primary -ne 'video') { $failed = "the main output is '$($face.primary)', not video" }
    }
    if (-not $failed) {
      $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
      if (-not $valid.result.ok) { $failed = 'the project is not valid after the edits' }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: rows of the Clip Inputs and Output nodes are renamed, moved, made main and removed, and an input added on the graph is on the card (captures in $work)" -ForegroundColor Green
exit 0
