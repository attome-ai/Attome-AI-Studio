# UI test: add a node at the pointer. A double click on the canvas opens the Node Library as a search box there; typing narrows it and
# a click (or Enter) puts the node where the box was. A link dragged from a port and let go on nothing opens the same box with only the
# nodes that fit its Data Type, and the new node comes linked. The left column is not touched. Mock engine, virtual input only.
#   .\tools\uitest\workflow_search.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Search.attome'
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
  function Count-Kind($w, $kind) { @($w.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq $kind }).Count }
  function Has-Link($w, $fromKind, $fromPort, $toKind, $toPort) {
    foreach ($l in $w.links.PSObject.Properties) {
      $f = $w.nodes.($l.Value.from[0]).kind; $t = $w.nodes.($l.Value.to[0]).kind
      if ($f -eq $fromKind -and $l.Value.from[1] -eq $fromPort -and $t -eq $toKind -and $l.Value.to[1] -eq $toPort) { return $true }
    }
    $false
  }

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 180 -Script @(
    'dblclick @clip:First'
    'expect @node:sample'
    'wait 300'
    'dblclick @workflow_canvas@0.5,0.85'    # on nothing: the search, where the pointer is
    'expect @field:wf_search'
    'type clip'
    'wait 200'
    "shot $work\search_open.jpg"
    'expect @wf_search:clip'
    'expect @wf_search:clip_reference'      # "clip" is in both
    'click @wf_search:clip'
    'wait 500'
    "shot $work\search_added.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $w = Get-Workflow $run
      $placed = @($w.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq 'attome.clip' })
      "after the search: $(Count-Kind $w 'attome.clip') Clip node, placed at $($placed[0].Value.ui.x),$($placed[0].Value.ui.y)"
      if ($placed.Count -ne 1) { $failed = 'the Clip node was not added' }
      elseif (-not $placed[0].Value.ui) { $failed = 'the new node has no place: it should be where the box was opened' }
    }
  } finally { }

  if (-not $failed) { # a link let go on nothing: the box lists what fits, and the node comes linked
    $second = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 180 -Script @(
      'dblclick @clip:First'
      'expect @node:decode'
      'drag @port:decode.video:out @workflow_canvas@0.7,0.9'   # a video, dropped on nothing
      'expect @field:wf_search'
      "shot $work\search_linked.jpg"
      'expect @wf_search:get_frame'          # a node that takes a video
      'click @wf_search:get_frame'
      'wait 500'
      'drag @port:sample.end_image @workflow_canvas@0.3,0.92'  # an input that nothing feeds: what could give a picture
      'expect @field:wf_search'
      'expect @wf_search:get_frame'
      'type frame'
      'key Enter'                            # Enter takes the first
      'wait 500'
      "shot $work\search_after.jpg"
    )
    $failed = $second.Errors
    if (-not $failed) {
      $w = Get-Workflow $run
      "get_frame nodes: $(Count-Kind $w 'attome.get_frame') (one was there before)"
      if ((Count-Kind $w 'attome.get_frame') -ne 3) { $failed = "expected 3 Get Frame nodes (one there, two added), found $(Count-Kind $w 'attome.get_frame')" }
      elseif (@($w.links.PSObject.Properties).Count -lt 5) { $failed = 'the new nodes were not linked' }
      elseif (-not (Has-Link $w 'attome.get_frame' 'image' 'attome.sample' 'end_image')) { $failed = 'the picture of the new node does not feed the sampler''s end_image' }
      else {
        $videoLinks = @($w.links.PSObject.Properties | Where-Object { $w.nodes.($_.Value.from[0]).kind -eq 'attome.decode' -and $_.Value.from[1] -eq 'video' }).Count
        if ($videoLinks -lt 2) { $failed = 'the new node is not linked to the decoder''s video' }
      }
    }
    if (-not $failed) {
      $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
      if (-not $valid.result.ok) { $failed = 'the project is not valid after the edits' }
    }
  }
  Stop-Daemon $run
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: nodes are added from a search at the pointer, and a link let go on nothing offers only what fits and links it (captures in $work)" -ForegroundColor Green
exit 0
