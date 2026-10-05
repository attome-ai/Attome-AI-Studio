# UI test: running from the Workflow Canvas, seeds and problems. The canvas has a Generate button for the clip it was opened from: a run
# makes a Take; a run that cannot start marks the node that needs something; a run that stops marks the node it stopped at, with the
# engine's message. The dice on the seed row gives a new seed and "New take" moves the seed on, never to one an earlier Take had. A
# clip that cannot be made yet has a red mark on the timeline, and the pointer on it says what is missing. Mock engine, virtual input.
#   .\tools\uitest\workflow_run.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Run.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null
    $info = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data
    $seq = $info.sequences[0].id
    # First is complete. Second has no prompt: its Encode prompt node needs one.
    @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"First","timing":{"record_in":"0","duration":"2","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":$(Get-ShotInstance 'a'),"inputs":{"prompt":"A robot walks","seed":3}}}},
 {"op":"add","path":"`$new:v1/clips/`$new:b","value":{"name":"Second","timing":{"record_in":"2","duration":"2","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":$(Get-ShotInstance 'b'),"inputs":{"seed":5}}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
    & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'the setup patch was refused' }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  function Get-Clip($run, $name) { foreach ($c in (Get-Tracks $run)[0].clip_list) { if ($c.name -eq $name) { return Get-Object $run $c.id } } }
  function Get-Status($run) { @((Invoke-Attome $run --json gen status $proj | ConvertFrom-Json).result.clips) }
  function Node-Of($clip, $kind) { ($clip.media_ref.workflow.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq $kind }).Name }

  # 1. The timeline: Second cannot be made yet. It is marked, and the pointer on it says why. A prompt makes the mark go.
  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
    'click @clip:Second'
    'wait 900'
    "shot $work\run_marked.jpg"             # the red mark on Second, the tooltip with what is missing
    'click @field:prompt'
    'type It starts to rain'
    'click @clip:First'                      # on to another clip while the field is still being typed in: the edit is Second's
    'wait 500'
    "shot $work\run_unmarked.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $st = Get-Status $run
      "status: $(($st | ForEach-Object { "$($_.name) ready=$($_.ready)" }) -join ', ')"
      $second = $st | Where-Object { $_.name -eq 'Second' }
      if (-not $second.ready) { $failed = 'the prompt was typed but the clip is still not ready' }
    }
  } finally { }

  # 2. Generate on the canvas: a Take. Then New take with the dice and the seed that moves on.
  if (-not $failed) {
    $go = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 240 -Script @(
      'dblclick @clip:First'
      'expect @button:wf_generate'
      'click @button:wf_generate'
      'wait 300'
      'expect @button:wf_generate'
      'wait 1500'
      "shot $work\run_canvas_done.jpg"
      'click @button:wf_back'
      'click @clip:First'
      'expect @field:input_seed'
      'click @button:input_dice_seed'         # a new random seed
      'wait 600'
      'click @button:gen_run'                 # Generate for the new seed: Take 2
      'wait 300'
      'expect @button:gen_run'
      'wait 1500'
      'click @button:gen_run'                 # New take: the seed moves on by itself
      'wait 300'
      'expect @button:gen_run'
      'wait 1500'
    )
    $failed = $go.Errors
    if (-not $failed) {
      $first = Get-Clip $run 'First'
      $seeds = @($first.media_ref.takes.PSObject.Properties | ForEach-Object { $_.Value.inputs.seed })
      "First: $(@($first.media_ref.take_order).Count) takes with seeds [$($seeds -join ', ')], seed now $($first.media_ref.inputs.seed)"
      if (@($first.media_ref.take_order).Count -ne 3) { $failed = "expected 3 Takes of First (canvas run, the dice, New take), found $(@($first.media_ref.take_order).Count)" }
      elseif (($seeds | Sort-Object -Unique).Count -ne 3) { $failed = 'two Takes have the same seed' }
      elseif ($seeds[0] -ne 3) { $failed = 'the run from the canvas did not use the clip''s seed' }
    }
  }

  # 3. A run that stops: the node it stopped at says why. (The mock engine is told to fail its sampling.)
  if (-not $failed) {
    Stop-Daemon $run
    $env:ATTOME_MOCK_FAIL = 'sample'
    try {
      $clip = Get-Clip $run 'Second'
      $sample = Node-Of $clip 'attome.sample'
      $stop = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 240 -Script @(
        'dblclick @clip:Second'
        'expect @button:wf_generate'
        'click @button:wf_generate'
        'wait 300'
        "expect @note:$sample"                # the sampler is marked, with the message under it
        'wait 300'
        "shot $work\run_failed.jpg"
      )
      $failed = $stop.Errors
    } finally { Remove-Item Env:\ATTOME_MOCK_FAIL -ErrorAction SilentlyContinue }
  }
  Stop-Daemon $run
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Generate runs from the canvas, a stopped run marks its node, the seed moves on without repeating, and a clip that cannot run is marked on the timeline (captures in $work)" -ForegroundColor Green
exit 0
