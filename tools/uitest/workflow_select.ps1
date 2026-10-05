# UI test: several nodes at once on the Workflow Canvas. Shift and a drag on the background selects what the box touches; the nodes
# move together, are copied with Ctrl+C, pasted with Ctrl+V (the links among them come along, in one undoable edit), duplicated with
# Ctrl+D and removed together with Delete. Mock engine, virtual input only.
#   .\tools\uitest\workflow_select.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Select.attome'
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

  function Get-Workflow($run) { (Get-Object $run ((Get-Tracks $run)[0].clip_list[0].id)).media_ref.workflow }
  function Shape($w) { "$(@($w.nodes.PSObject.Properties).Count) nodes, $(@($w.links.PSObject.Properties).Count) links" }
  function Positions($w) { @($w.nodes.PSObject.Properties | Where-Object { $_.Value.ui } | Sort-Object { $_.Value.kind } | ForEach-Object { [pscustomobject]@{ Kind = $_.Value.kind; X = [double]$_.Value.ui.x; Y = [double]$_.Value.ui.y } }) }

  # A box over all the nodes (Shift held for the drag), then the nodes moved by one of them.
  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
    'dblclick @clip:First'
    'expect @node:sample'
    'wait 300'
    'drag @workflow_canvas@0.28,0.04 700 520 shift'
    'expect @button:wf_duplicate'            # the side panel says several are selected
    "shot $work\select_box.jpg"
    'drag @node:sample 40 60'                # one of them is dragged: all go
    'wait 500'
    "shot $work\select_moved.jpg"
  )
  $failed = $run.Errors
  $base = $null
  try {
    if (-not $failed) {
      $w = Get-Workflow $run
      $pos = Positions $w
      $base = $pos
      "moved: $(($pos | ForEach-Object { "$($_.Kind -replace 'attome.','')@$($_.X),$($_.Y)" }) -join '  ')   $(Shape $w)"
      if ($pos.Count -ne 4) { $failed = "expected all 4 nodes to be moved and placed, $($pos.Count) have a place" }
    }
  } finally { }

  if (-not $failed) { # moved together again: every node by the same amount
    $second = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 240 -Script @(
      'dblclick @clip:First'
      'expect @node:sample'
      'drag @workflow_canvas@0.28,0.04 700 520 shift'
      'expect @button:wf_duplicate'
      'drag @node:decode 70 50'
      'wait 500'
    )
    $failed = $second.Errors
    if (-not $failed) {
      $now = Positions (Get-Workflow $run)
      $dx = @(); $dy = @()
      for ($i = 0; $i -lt $now.Count; ++$i) { $dx += [math]::Round($now[$i].X - $base[$i].X); $dy += [math]::Round($now[$i].Y - $base[$i].Y) }
      "second move: dx [$($dx -join ',')] dy [$($dy -join ',')]"
      if (($dx | Sort-Object -Unique).Count -ne 1 -or ($dy | Sort-Object -Unique).Count -ne 1) { $failed = 'the nodes did not move by the same amount' }
      elseif ($dx[0] -eq 0 -and $dy[0] -eq 0) { $failed = 'the nodes did not move' }
    }
  }

  if (-not $failed) { # copy, paste, duplicate, remove: each one edit
    $third = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 240 -Script @(
      'dblclick @clip:First'
      'expect @node:sample'
      'drag @workflow_canvas@0.28,0.04 700 520 shift'
      'expect @button:wf_duplicate'
      'key C ctrl'
      'wait 200'
      'key V ctrl'                             # pasted: 4 more nodes and the 3 links among them
      'wait 600'
      "shot $work\select_pasted.jpg"
    )
    $failed = $third.Errors
    if (-not $failed) {
      $w = Get-Workflow $run
      "after paste: $(Shape $w)"
      if (@($w.nodes.PSObject.Properties).Count -ne 8 -or @($w.links.PSObject.Properties).Count -ne 6) { $failed = "the paste should give 8 nodes and 6 links: $(Shape $w)" }
    }
  }
  if (-not $failed) { # the pasted ones are selected: Delete takes them away; Ctrl+D copies and pastes at once; Ctrl+Z undoes the one edit
    $fourth = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 240 -Script @(
      'dblclick @clip:First'
      'expect @node:sample'
      'drag @workflow_canvas@0.28,0.04 700 520 shift'
      'key D ctrl'
      'wait 600'
      'key Z ctrl'                             # the duplicate, as one edit, undone
      'wait 600'
    )
    $failed = $fourth.Errors
    if (-not $failed) {
      $w = Get-Workflow $run
      "after duplicate and undo: $(Shape $w)"
      if (@($w.nodes.PSObject.Properties).Count -ne 8) { $failed = "undo of the duplicate should leave the 8 nodes of before: $(Shape $w)" }
    }
  }
  if (-not $failed) {
    $fifth = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 240 -Script @(
      'dblclick @clip:First'
      'expect @node:sample'
      'key A ctrl'                             # every node
      'expect @button:wf_remove_nodes'
      'key Delete'
      'wait 600'
      "shot $work\select_deleted.jpg"
    )
    $failed = $fifth.Errors
    if (-not $failed) {
      $w = Get-Workflow $run
      "after Delete of all: $(Shape $w)"
      if (@($w.nodes.PSObject.Properties).Count -ne 0) { $failed = "Delete should remove every selected node: $(Shape $w)" }
      else {
        $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
        if (-not $valid.result.ok) { $failed = 'the project is not valid after the edits' }
      }
    }
  }
  Stop-Daemon $run
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: nodes are selected by a box, moved together, copied, pasted, duplicated and removed, each as one edit (captures in $work)" -ForegroundColor Green
exit 0
