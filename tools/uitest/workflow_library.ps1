# UI test: the Template Library in the Generate panel. A clip's own workflow is saved to the library from its card; the library
# workflow is a card in the Generate panel, dragged onto the timeline; the new clip has its own copy, so editing it leaves the
# first clip (and the library) as they were; "Reset to library" puts an edited clip back. Mock engine, virtual input only.
#   .\tools\uitest\workflow_library.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Library.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try { & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  function Get-Clips($run) { @((Get-Tracks $run) | ForEach-Object { $_.clip_list } | ForEach-Object { Get-Object $run $_.id } | Sort-Object { ConvertFrom-Rational $_.timing.record_in }) }
  function Get-Gen($w) { ($w.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq 'attome.generate_video' }).Name }

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
    'click @rail:Generate'
    'click @model:attome-mock'               # the first clip, from the built-in Shot
    'wait 600'
    'click @field:prompt'
    'type A robot walks'
    'click @rail:Generate'
    'wait 400'
    'click @button:input_add'                # the clip gets an input of its own, so the workflow is worth saving
    'click @field:new_input_name'
    'type Mood'
    'click @button:input_add_ok'
    'wait 400'
    'click @button:workflow_save'            # its workflow becomes a card
    'wait 600'
    'click @rail:Generate'
    'expect @workflow_card:Shot'
    "shot $work\library_panel.jpg"
    'drag @workflow_card:Shot @clip:Shot_1@0.9,0.5'   # onto the timeline, after the first clip
    'wait 800'
    "shot $work\library_dropped.jpg"
    'expect @input:mood'                     # the new clip has the input the saved workflow has
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $clips = Get-Clips $run
      $project = (Invoke-Attome $run --json get $proj (Invoke-Attome $run --json inspect $proj | ConvertFrom-Json).result.data.id | ConvertFrom-Json).result.object
      $lib = @($project.workflows.PSObject.Properties)
      "clips: $(($clips | ForEach-Object { "$($_.name)@$($_.timing.record_in)" }) -join ', ')   library: $(($lib | ForEach-Object { $_.Value.name }) -join ', ')"
      if ($clips.Count -ne 2) { $failed = "expected 2 clips, found $($clips.Count)" }
      elseif ($lib.Count -ne 1) { $failed = 'the workflow is not in the library' }
      elseif ($clips[1].media_ref.workflow.source -ne $lib[0].Name) { $failed = 'the new clip does not say it comes from the library workflow' }
      elseif ((Get-Gen $clips[0].media_ref.workflow) -eq (Get-Gen $clips[1].media_ref.workflow)) { $failed = 'the two clips share a node: they must each have their own' }
      elseif (-not ($clips[1].media_ref.workflow.exposed.inputs.PSObject.Properties.Name -contains 'mood')) { $failed = 'the new clip lacks the saved input' }
    }
  } finally { }

  if (-not $failed) { # edit the second clip's own workflow; the first and the library stay; then reset it
    $before = (Get-Clips $run)[0] | ConvertTo-Json -Depth 30 -Compress
    $second = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 180 -Script @(
      'click @clip:Shot_2'
      'wait 400'
      'click @button:input_remove_mood'    # the second clip drops the input
      'wait 500'
      "shot $work\library_edited.jpg"
      'click @button:workflow_reset'       # and gets it back from the library
      'wait 600'
      'expect @input:mood'
      "shot $work\library_reset.jpg"
    )
    $failed = $second.Errors
    if (-not $failed) {
      $clips = Get-Clips $run
      $after = $clips[0] | ConvertTo-Json -Depth 30 -Compress
      if ($before -ne $after) { $failed = 'editing the second clip changed the first' }
      elseif (-not ($clips[1].media_ref.workflow.exposed.inputs.PSObject.Properties.Name -contains 'mood')) { $failed = 'the reset did not bring the input back' }
      else {
        $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
        if (-not $valid.result.ok) { $failed = 'the project is not valid after the reset' }
      }
    }
  }
  Stop-Daemon $run
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a workflow saved to the library is a card in the Generate panel; its clips have their own copies; Reset puts one back (captures in $work)" -ForegroundColor Green
exit 0
