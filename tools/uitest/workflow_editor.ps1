# UI test: the workflow editor. A double click on a generative clip opens its workflow as a graph. A new workflow is then
# built by hand: three nodes added, linked by dragging from an output to an input, given a model, with the prompt set by
# the clip and the video given to the clip. The engine must accept every step (a graph that is not finished is not an
# error), and the finished workflow must be valid. Mock engine, virtual input only.
#   .\tools\uitest\workflow_editor.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Workflow.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try { & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 150 -Script @(
    'click @rail:Generate'
    'click @model:attome-mock'               # a generative clip, with the built-in Shot workflow of the model
    'wait 600'
    'dblclick @clip:Shot_1'                  # its workflow, as a graph
    'expect @node:generate_video'
    'wait 300'
    "shot $work\workflow_shot.jpg"
    'click @node:generate_video'             # the node's model, settings and inputs on the right
    'expect @combo:wf_model'
    'wait 200'
    "shot $work\workflow_node.jpg"
    'click @button:wf_new'                   # a workflow built by hand
    'wait 400'
    'click @button:wf_add_encode_prompt'
    'wait 300'
    'click @button:wf_add_sample'
    'wait 300'
    'click @button:wf_add_decode'
    'wait 300'
    "shot $work\workflow_loose.jpg"          # three nodes, nothing linked: red where an input has nothing behind it
    'drag @port:encode_prompt.conditioning:out @port:sample.conditioning'
    'wait 400'
    'drag @port:sample.latent:out @port:decode.latent'
    'wait 400'
    'click @node:encode_prompt'
    'click @combo:wf_model'
    'click @wfmodel:attome-mock'
    'wait 300'
    'click @check:wf_clip_sets_prompt'       # the prompt comes from the clip
    'wait 300'
    'click @node:sample'
    'click @combo:wf_model'
    'click @wfmodel:attome-mock'
    'wait 300'
    'click @node:decode'
    'click @combo:wf_model'
    'click @wfmodel:attome-mock'
    'wait 300'
    'click @check:wf_clip_gets_video'        # the clip plays what the decoder makes
    'wait 300'
    'drag @node:decode 40 120'               # a node moved: its place is kept
    'wait 400'
    "shot $work\workflow_built.jpg"
    'click @button:wf_back'
    'expect @clip:Shot_1'
    'wait 200'
    "shot $work\workflow_back.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $project = (Invoke-Attome $run --json get $proj (Invoke-Attome $run --json inspect $proj | ConvertFrom-Json).result.data.id | ConvertFrom-Json).result.object
      $made = $project.workflows.PSObject.Properties | Where-Object { -not $_.Value.builtin } | Select-Object -First 1
      if (-not $made) { $failed = 'the new workflow is not in the project' }
      else {
        $w = $made.Value
        $nodes = @($w.nodes.PSObject.Properties)
        $links = @($w.links.PSObject.Properties)
        $kinds = ($nodes | ForEach-Object { $_.Value.kind } | Sort-Object) -join ','
        $models = @($nodes | Where-Object { $_.Value.model -eq 'attome-mock' }).Count
        $placed = @($nodes | Where-Object { $_.Value.ui }).Count
        "workflow: nodes [$kinds], $($links.Count) links, $models with the model, $placed placed; sets [$(@($w.exposed.inputs.PSObject.Properties.Name) -join ',')] gets [$(@($w.exposed.outputs.PSObject.Properties.Name) -join ',')]"
        if ($kinds -ne 'attome.decode,attome.encode_prompt,attome.sample') { $failed = 'the three nodes were not added' }
        elseif ($links.Count -ne 2) { $failed = "expected 2 links, found $($links.Count)" }
        elseif ($models -ne 3) { $failed = "expected the model on all 3 nodes, found it on $models" }
        elseif ((@($w.exposed.inputs.PSObject.Properties.Name) -join ',') -ne 'prompt') { $failed = 'the prompt is not set by the clip' }
        elseif ((@($w.exposed.outputs.PSObject.Properties.Name) -join ',') -ne 'video') { $failed = 'the video is not given to the clip' }
        elseif ($placed -ne 1) { $failed = "expected the place of one node to be kept, found $placed" }
      }
    }
    if (-not $failed) {
      $valid = (Invoke-Attome $run --json validate $proj | ConvertFrom-Json)
      "validate: ok=$($valid.result.ok) warnings=$(@($valid.result.warnings).Count)"
      if (-not $valid.result.ok) { $failed = 'the project is not valid after the edits' }
      elseif (@($valid.result.warnings | Where-Object { $_.rule -eq 'G_MISSING' }).Count) { $failed = 'the finished workflow still has an input with nothing behind it' }
    }
    if (-not $failed) { # moving a node is not a change to what the clip makes
      $clip = (Invoke-Attome $run --json gen status $proj | ConvertFrom-Json).result.clips[0]
      "the clip: $($clip.name) $($clip.state) ready=$($clip.ready)"
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a clip's workflow opens as a graph, and a workflow was built by adding, linking and setting nodes (captures in $work)" -ForegroundColor Green
exit 0
