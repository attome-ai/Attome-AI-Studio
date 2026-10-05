# UI test: the Input nodes. A clip's workflow reads the project and the timeline through nodes: a Variable node gives its prompt,
# a Project node its size, a Clip node its length, and a Clip Reference node and a Get Frame node the last frame of the clip
# before. The graph shows them as one family; the Inspector chooses the Variable and the clip; the clip is generated, and a
# change of the Variable makes that clip, and no other, out of date. Mock engine, virtual input only.
#   .\tools\uitest\workflow_inputs.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Inputs.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# The second clip's workflow: the prompt from a Variable, the size from the Project, the length from the Clip, and the start
# picture from the last frame of the clip before. Nothing about the timeline is held in it.
function Get-InputsInstance {
  $m = 'attome-mock'
  @"
{"name":"Shot","source":"test",
 "nodes":{
  "`$new:enc":{"kind":"attome.encode_prompt","model":"$m"},
  "`$new:smp":{"kind":"attome.sample","model":"$m","settings":{"steps":4}},
  "`$new:dec":{"kind":"attome.decode","model":"$m"},
  "`$new:frm":{"kind":"attome.get_frame","settings":{"frame":"last"}},
  "`$new:sty":{"kind":"attome.variable","variable":"`$new:style","type":"text"},
  "`$new:cvs":{"kind":"attome.project","settings":{"pixels":901120}},
  "`$new:clk":{"kind":"attome.clip"},
  "`$new:ref":{"kind":"attome.clip_reference","settings":{"clip":"previous"}},
  "`$new:prev":{"kind":"attome.get_frame","settings":{"frame":"last"}}},
 "links":{
  "`$new:l1":{"from":["`$new:enc","conditioning"],"to":["`$new:smp","conditioning"]},
  "`$new:l2":{"from":["`$new:smp","latent"],"to":["`$new:dec","latent"]},
  "`$new:l3":{"from":["`$new:dec","video"],"to":["`$new:frm","video"]},
  "`$new:l4":{"from":["`$new:sty","value"],"to":["`$new:enc","prompt"]},
  "`$new:l5":{"from":["`$new:cvs","width"],"to":["`$new:smp","width"]},
  "`$new:l6":{"from":["`$new:cvs","height"],"to":["`$new:smp","height"]},
  "`$new:l7":{"from":["`$new:clk","duration"],"to":["`$new:smp","seconds"]},
  "`$new:l8":{"from":["`$new:ref","video"],"to":["`$new:prev","video"]},
  "`$new:l9":{"from":["`$new:prev","image"],"to":["`$new:smp","start_image"]}},
 "exposed":{"inputs":{"seed":{"type":"integer","order":0,"to":[["`$new:smp","seed"]]},"mood":{"type":"text","order":1},"start_image":{"type":"image","order":2}},
  "outputs":{"video":{"from":["`$new:dec","video"]},"last_frame":{"from":["`$new:frm","image"]}},"primary":"video"}}
"@
}

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null
    $info = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data
    $prj = $info.id; $seq = $info.sequences[0].id
    @"
{"ops":[
 {"op":"add","path":"$prj/variables/`$new:style","value":{"name":"style","type":"text","value":"anime"}},
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"First","timing":{"record_in":"0","duration":"2","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":$(Get-ShotInstance 'a'),"inputs":{"prompt":"A robot walks","seed":3}}}},
 {"op":"add","path":"`$new:v1/clips/`$new:b","value":{"name":"Second","timing":{"record_in":"2","duration":"2","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":$(Get-InputsInstance),"inputs":{"seed":5,"mood":"calm"}}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
    & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'the setup patch was refused' }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 180 -Script @(
    'click @clip:Second'
    'wait 400'
    'expect @slider:gen_length'             # the length of the clip card is the Duration the Clip node gives
    'expect @combo:gen_start'               # and what it starts from is the Clip Reference node
    "shot $work\inputs_card.jpg"
    'dblclick @clip:Second'
    'expect @node:variable'
    'expect @node:project'
    'expect @node:clip'
    'expect @node:clip_reference'
    'wait 300'
    "shot $work\inputs_graph.jpg"           # the Input nodes in one colour, each saying what it gives
    'click @node:clip_reference'
    'expect @combo:wf_reference'
    'click @combo:wf_reference'
    'click @wfreference:First'              # the clip it reads: now named, not "the clip before"
    'wait 400'
    "shot $work\inputs_reference.jpg"
    'click @node:variable'
    'expect @combo:wf_variable'
    'wait 200'
    "shot $work\inputs_variable.jpg"
    'click @button:wf_back'
    'click @clip:Second'
    'wait 400'
    'click @button:gen_run'                 # the first clip is made too: the second reads its video
    'wait 300'
    'expect @button:gen_run'
    'wait 1800'
    "shot $work\inputs_done.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $clips = @((Invoke-Attome $run --json gen status $proj | ConvertFrom-Json).result.clips)
      "status: $(($clips | ForEach-Object { "$($_.name)=$($_.state) takes=$($_.takes) depends=$(@($_.depends_on).Count)" }) -join ', ')"
      $a = $clips | Where-Object { $_.name -eq 'First' }; $b = $clips | Where-Object { $_.name -eq 'Second' }
      $ref = (Get-Object $run $b.clip).media_ref.workflow.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq 'attome.clip_reference' }
      "the reference reads: $($ref.Value.settings.clip) (First is $($a.clip))"
      if ($ref.Value.settings.clip -ne $a.clip) { $failed = 'the Clip Reference node was not set to the first clip' }
      elseif (@($clips | Where-Object { $_.state -ne 'clean' -or $_.takes -ne 1 }).Count) { $failed = 'both clips are not generated' }
      elseif (@($b.depends_on).Count -ne 1 -or $b.depends_on[0] -ne $a.clip) { $failed = 'the second clip does not depend on the first through its Clip Reference node' }
    }
    if (-not $failed) { # the Variable: change it once; exactly the clip that reads it is out of date
      $var = ((Invoke-Attome $run --json get $proj $prj | ConvertFrom-Json).result.object.variables.PSObject.Properties | Select-Object -First 1).Name
      '{"ops":[{"op":"replace","path":"' + $var + '/value","value":"film"}]}' | Set-Content "$work\var.json" -Encoding utf8
      $changed = Invoke-Attome $run --json patch $proj "$work\var.json" | ConvertFrom-Json
      $after = @((Invoke-Attome $run --json gen status $proj | ConvertFrom-Json).result.clips)
      "after the Variable changed: $(($after | ForEach-Object { "$($_.name)=$($_.state)" }) -join ', ')"
      if (-not $changed.ok) { $failed = 'the change of the Variable was refused' }
      elseif (($after | Where-Object { $_.name -eq 'First' }).state -ne 'clean') { $failed = 'the first clip does not read the Variable and is out of date' }
      elseif (($after | Where-Object { $_.name -eq 'Second' }).state -ne 'dirty') { $failed = 'the clip that reads the Variable is not out of date' }
    }
    if (-not $failed) {
      $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
      if (-not $valid.result.ok) { $failed = 'the project is not valid' }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Input nodes read a Variable, the project and the clip before; one change of the Variable made exactly that clip out of date (captures in $work)" -ForegroundColor Green
exit 0
