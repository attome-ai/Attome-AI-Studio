# UI test: the dots of the workflow graph, as in ComfyUI. A connected input dragged away and let go on nothing breaks the
# connection; dragged to another input it moves there. The clip's "sets" side is wired by dragging too. The Inspector's
# Disconnect breaks one, and an input with nothing joined takes a typed value of its own type. Captures while a wire is
# held. Mock engine, virtual input only.
#   .\tools\uitest\workflow_wires.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Wires.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# A workflow of three blocks, linked: encode -> sample -> decode. The clip sets the prompt and the seed and gets the video.
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
   "media_ref":{"type":"workflow","workflow":$(Get-ShotInstance -Exposed @('prompt', 'seed')),"inputs":{"prompt":"A robot walks","seed":3}}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
    & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'the setup patch was refused' }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  function Get-Workflow($run) { # the clip's own workflow
    $clip = (Get-Tracks $run)[0].clip_list[0].id
    (Get-Object $run $clip).media_ref.workflow
  }
  function Describe($w) {
    $links = @($w.links.PSObject.Properties | ForEach-Object { "$($w.nodes.($_.Value.from[0]).kind -replace 'attome.','').$($_.Value.from[1])>$($w.nodes.($_.Value.to[0]).kind -replace 'attome.','').$($_.Value.to[1])" })
    $sets = @($w.exposed.inputs.PSObject.Properties | ForEach-Object {
      $name = $_.Name
      if (@($_.Value.to).Count) { foreach ($t in $_.Value.to) { "$name>$($w.nodes.($t[0]).kind -replace 'attome.','').$($t[1])" } } else { "$name>(unlinked)" }
    })
    "links [$($links -join ', ')]  clip sets [$($sets -join ', ')]"
  }

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 150 -Script @(
    'dblclick @clip:First'
    'expect @node:sample'
    'wait 300'
    "shot $work\wires_start.jpg"
    'drag @port:sample.seed @node:decode hold'   # the clip's seed picked up from where it goes in
    'wait 200'
    "shot $work\wires_held.jpg"                  # the wire follows the pointer; inputs it may go to are ringed
    'release'                                   # let go on a node, not a dot: the connection is broken
    'wait 500'
    "shot $work\wires_cut.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $w = Get-Workflow $run; "1. seed dropped on nothing: $(Describe $w)"
      if (-not ($w.exposed.inputs.PSObject.Properties.Name -contains 'seed')) { $failed = '1. the Exposed Input "seed" is gone: cutting a feed must leave it, unlinked' }
      elseif (@($w.exposed.inputs.seed.to).Count -ne 0) { $failed = '1. the seed still feeds the sampler' }
      elseif (@($w.links.PSObject.Properties).Count -ne 3) { $failed = '1. a link changed' }
      else {
        $clip = (Get-Tracks $run)[0].clip_list[0].id
        if (-not ((Get-Object $run $clip).media_ref.inputs.PSObject.Properties.Name -contains 'seed')) { $failed = '1. the clip lost its value for the unlinked seed' }
      }
    }
    if (-not $failed) { # the link into the decoder picked up and let go on nothing; then put back from the output
      $more = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
        'dblclick @clip:First'
        'expect @node:decode'
        'drag @port:decode.latent @node:encode_prompt'
        'wait 500'
        "shot $work\wires_link_cut.jpg"           # the decoder's input is red: nothing behind it
        'drag @port:sample.latent:out @port:decode.latent'
        'wait 500'
        'drag @clipin:seed @port:sample.seed'     # the unlinked seed fed to the sampler again, from its row on the clip's side
        'wait 500'
        "shot $work\wires_back.jpg"
      )
      $failed = $more.Errors
      if (-not $failed) {
        $w = Get-Workflow $run; "2. cut, linked again, seed wired from the clip's side: $(Describe $w)"
        if (@($w.links.PSObject.Properties).Count -ne 3) { $failed = '2. the decoder is not linked again' }
        elseif (@($w.exposed.inputs.seed.to).Count -ne 1) { $failed = '2. the seed does not feed the sampler again' }
      }
    }
    if (-not $failed) { # the Inspector: Disconnect, then a typed value of the input's type
      $side = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
        'dblclick @clip:First'
        'click @node:sample'
        'expect @button:wf_cut_seed'
        'click @button:wf_cut_seed'
        'wait 500'
        'click @field:wfin_seed'
        'type 42'
        'click @node:sample'                       # leaving the field saves it
        'wait 500'
        "shot $work\wires_typed.jpg"
      )
      $failed = $side.Errors
      if (-not $failed) {
        $w = Get-Workflow $run
        $seed = ($w.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq 'attome.sample' }).Value.inputs.seed
        "3. Disconnect and a typed seed: $(Describe $w)  seed=$seed"
        if (@($w.exposed.inputs.seed.to).Count -ne 0) { $failed = '3. Disconnect did not unlink the seed' }
        elseif ($seed -ne 42) { $failed = "3. the typed seed is $seed, not 42" }
      }
    }
    if (-not $failed) {
      $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
      if (-not $valid.result.ok) { $failed = 'the project is not valid after the edits' }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: wires are picked up and broken by dropping on nothing, put back by dragging, and cut and typed from the Inspector (captures in $work)" -ForegroundColor Green
exit 0
