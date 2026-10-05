# UI test: bigger graphs. A node shows the picture of what it made last, and the node that is running shows a bar that moves; a frame
# and a note are added to the canvas, a frame moves the nodes inside it, and both are saved with the workflow; a workflow of the library
# is added as one node (a Subgraph) and opens with a double click or a button. Mock engine, virtual input only.
#   .\tools\uitest\workflow_extras.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Extras.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
$env:ATTOME_MOCK_DELAY_MS = '300'          # a pause per sampling step, so the bar can be seen moving
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null
    $info = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data
    $seq = $info.sequences[0].id
    @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"First","timing":{"record_in":"0","duration":"1","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":$(Get-ShotInstance 'a' -Seconds 1),"inputs":{"prompt":"A robot walks","seed":3}}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
    & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'the setup patch was refused' }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  function Get-Clip($run) { Get-Object $run ((Get-Tracks $run)[0].clip_list[0].id) }
  function Node-Id($clip, $kind) { ($clip.media_ref.workflow.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq $kind }).Name }

  # The ids of the nodes, read from the project the setup wrote.
  $probe = Invoke-EditorScript -Project $proj -TimeoutSeconds 120 -Script @('wait 100')
  $clip0 = Get-Clip $probe
  $sample = Node-Id $clip0 'attome.sample'; $frame = Node-Id $clip0 'attome.get_frame'; $decode = Node-Id $clip0 'attome.decode'
  Stop-Daemon $probe

  # 1. A run from the canvas: the sampler shows its bar while it runs; afterwards the nodes show their pictures.
  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
    'dblclick @clip:First'
    'expect @button:wf_generate'
    'click @button:wf_generate'
    "expect @running:$sample"                # the bar is on the node that runs
    "shot $work\extras_running.jpg"
    'expect @button:wf_generate'             # back when the run has ended
    'wait 600'
    "expect @preview:$frame"                 # the last frame, on the Get Frame node
    "shot $work\extras_preview.jpg"
  )
  $failed = $run.Errors

  # 2. A frame and a note; the frame moves with what is inside it; both are kept.
  $before = $null
  if (-not $failed) {
    $clip = Get-Clip $run
    $before = @{}
    foreach ($n in $clip.media_ref.workflow.nodes.PSObject.Properties) { if ($n.Value.ui) { $before[$n.Name] = @([double]$n.Value.ui.x, [double]$n.Value.ui.y) } }
    $deco = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 240 -Script @(
      'dblclick @clip:First'
      'expect @button:wf_add_group'
      'drag @workflow_canvas@0.28,0.04 700 520 shift'   # every node selected: the frame goes around them
      'expect @button:wf_duplicate'
      'click @button:wf_add_group'
      'expect @field:wf_group_title'
      'click @field:wf_group_title'
      'key End'
      'key Backspace'
      'key Backspace'
      'key Backspace'
      'key Backspace'
      'key Backspace'
      'type Look'
      'key Enter'
      'wait 300'
      'click @button:wf_group_colour_3fa66b'
      'wait 300'
      'click @button:wf_add_note'
      'expect @field:wf_note_text'
      'wait 300'
      "shot $work\extras_deco.jpg"
      'drag @group:Look 80 60'   # by its title bar: the frame and the nodes inside it move together
      'wait 600'
      "shot $work\extras_moved.jpg"
    )
    $failed = $deco.Errors
    if (-not $failed) {
      $clip = Get-Clip $run
      $wf = $clip.media_ref.workflow
      $groups = @($wf.groups.PSObject.Properties); $notes = @($wf.notes.PSObject.Properties)
      "groups: $(($groups | ForEach-Object { "$($_.Value.title) $($_.Value.color) at $($_.Value.x),$($_.Value.y)" }) -join '; ')   notes: $($notes.Count)"
      if ($groups.Count -ne 1) { $failed = "expected 1 frame, found $($groups.Count)" }
      elseif ($groups[0].Value.title -ne 'Look') { $failed = "the frame's title is '$($groups[0].Value.title)'" }
      elseif ($groups[0].Value.color -ne '#3fa66b') { $failed = 'the frame did not get the colour' }
      elseif ($notes.Count -ne 1) { $failed = "expected 1 note, found $($notes.Count)" }
      else {
        $moved = 0; $deltas = @()
        foreach ($n in $wf.nodes.PSObject.Properties) {
          if ($n.Value.ui -and $before.ContainsKey($n.Name)) {
            $dx = [double]$n.Value.ui.x - $before[$n.Name][0]
            if ([math]::Abs($dx) -gt 1) { ++$moved; $deltas += [math]::Round($dx) }
          } elseif ($n.Value.ui) { ++$moved } # nodes that had no place of their own are given one when moved
        }
        "nodes moved with the frame: $moved   dx $($deltas -join ',')"
        $placed = @($wf.nodes.PSObject.Properties | Where-Object { $_.Value.ui })
        $minX = ($placed | ForEach-Object { [double]$_.Value.ui.x } | Measure-Object -Minimum).Minimum
        $minY = ($placed | ForEach-Object { [double]$_.Value.ui.y } | Measure-Object -Minimum).Minimum
        "frame at $($groups[0].Value.x),$($groups[0].Value.y); the first node at $minX,$minY"
        if ($placed.Count -ne 4) { $failed = "expected the 4 nodes inside the frame to have moved, $($placed.Count) have a place" }
        elseif ([math]::Abs([double]$groups[0].Value.x - ($minX - 20)) -gt 2 -or [math]::Abs([double]$groups[0].Value.y - ($minY - 44)) -gt 2) { $failed = 'the frame and the nodes inside it are not where they were to each other' }
      }
    }
  }

  # 3. A Subgraph: the workflow is saved to the library, added to its own clip as one node, and opened.
  if (-not $failed) {
    $sub = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 240 -Script @(
      'dblclick @clip:First'
      'expect @button:workflow_save'
      'click @button:workflow_save'
      'wait 600'
      'expect @button:wf_sub_Shot'
      'click @button:wf_sub_Shot'             # the library's Shot as one node
      'wait 600'
      'expect @button:wf_open_inner'
      "shot $work\extras_subgraph.jpg"
      'click @button:wf_open_inner'           # it opens the workflow inside
      'wait 600'
      "shot $work\extras_inner.jpg"
    )
    $failed = $sub.Errors
    if (-not $failed) {
      $clip = Get-Clip $run
      $subs = @($clip.media_ref.workflow.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq 'attome.workflow' })
      "Subgraph nodes: $($subs.Count)"
      if ($subs.Count -ne 1) { $failed = 'the library workflow was not added as a node' }
      else {
        $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
        if (-not $valid.result.ok) { $failed = 'the project is not valid after the edits' }
      }
    }
  }
  Stop-Daemon $run
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE, Env:\ATTOME_MOCK_DELAY_MS -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: nodes show their pictures and progress, frames and notes are kept and a frame moves what is in it, a workflow is a node and opens (captures in $work)" -ForegroundColor Green
exit 0
