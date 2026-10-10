# UI test: nodes are chosen by key on the Workflow Canvas (plan docs/plan/KEYBOARD_WORKFLOW.md, M1). Home chooses the first node, the arrows the
# nearest one that way, Tab the next; F6 sends the keys to the side panel (a ring shows on it) and Esc brings them back; Delete takes the
# chosen node away. Mock engine, virtual input only. Look at the captures too.
#   .	oolsuitestworkflow_keys.ps1


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
  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
    'dblclick @clip:First'
    'expect @node:sample'
    'wait 400'
    'key Home', 'wait 300', "shot $work\keys_home.jpg"
    'key Right', 'wait 300', "shot $work\keys_right.jpg"
    'key Tab', 'wait 200', 'key Tab', 'wait 300', "shot $work\keys_tab.jpg"
    'key F6', 'wait 300', "shot $work\keys_side.jpg"
    'key Escape', 'wait 300'
    'key Home', 'key Right', 'wait 300'                                                    # Sample
    'key L', 'wait 300', 'expect @wf_port:latent', "shot $work\keys_ports.jpg"           # L: its outputs; Enter holds latent
    'key Enter', 'wait 400', 'key Right', 'wait 300', 'key I', 'wait 300'                  # the next node (Decode); I: the inputs that fit
    'expect @wf_port:latent', "shot $work\keys_ports2.jpg", 'key Enter', 'wait 800'       # Enter links: what fed Decode's latent gives way
    'key Home', 'wait 200', 'key Right alt', 'wait 500', 'key Right alt', 'wait 500'      # two grid steps: two edits
    "shot $work\keys_moved.jpg"
    'key A', 'wait 400', 'expect @field:wf_search'                                      # A: the search opens
    'type deco', 'wait 300', "shot $work\keys_search.jpg", 'key Enter', 'wait 800'      # and Enter adds the first node it shows
    "shot $work\keys_added.jpg"
    'key Delete', 'wait 800'                                                             # the node just added is the chosen one: it goes again
    "shot $work\keys_deleted.jpg"
    'key Home', 'wait 300', 'key BracketRight', 'wait 400', "shot $work\keys_link.jpg"    # [ and ]: a link of the chosen node is the selected one
    'key Delete', 'wait 800', "shot $work\keys_link_cut.jpg"                              # Delete takes that link away, not the node
    'click @button:wf_add_group', 'wait 600'                                              # a frame, chosen as it is made
    'key Right alt', 'wait 500', 'key Down alt', 'wait 500', "shot $work\keys_frame_moved.jpg"   # Alt+arrows move it
    'key Delete', 'wait 800'                                                              # and Delete removes it
    'click @button:wf_add_note', 'wait 600', 'key Delete', 'wait 800'                     # a note, the same
    'key Home', 'wait 200'
    'key Tab', 'key Tab', 'key Tab', 'key Tab', 'key Tab', 'key Tab', 'key Tab', 'wait 300', "shot $work\keys_row.jpg"   # on past the nodes: the rows of the clip's inputs
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $w = Get-Workflow $run
      "after the moves, the added node and Delete: $(Shape $w)"
      if (@($w.nodes.PSObject.Properties).Count -ne 4) { $failed = 'adding a node by key and deleting it should leave the four nodes' }
      $cuts = @((Invoke-Attome $run --json history $proj | ConvertFrom-Json).result.changesets | Where-Object { $_.label -eq 'Remove link' })
      "Remove link edits: $($cuts.Count)"
      if (-not $failed -and $cuts.Count -ne 1) { $failed = "] then Delete made $($cuts.Count) Remove link edits, not 1" }
      $gone = @((Invoke-Attome $run --json history $proj | ConvertFrom-Json).result.changesets | Where-Object { $_.label -in 'Remove frame', 'Remove note' })
      "Remove frame or note edits: $($gone.Count)"
      if (-not $failed -and $gone.Count -ne 2) { $failed = "Delete on a frame and a note made $($gone.Count) removals, not 2" }
      $links = @((Invoke-Attome $run --json history $proj | ConvertFrom-Json).result.changesets | Where-Object { $_.label -eq 'Link' })
      "Link edits: $($links.Count)"
      if (-not $failed -and $links.Count -ne 1) { $failed = "L, Enter, I, Enter made $($links.Count) Link edits, not 1" }
      $moves = @((Invoke-Attome $run --json history $proj | ConvertFrom-Json).result.changesets | Where-Object { $_.label -eq 'Move node' })
      "Move node edits: $($moves.Count)"
      if (-not $failed -and $moves.Count -lt 2) { $failed = "Alt+Right twice made $($moves.Count) Move node edits, not 2" }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }
if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: nodes are chosen by key; look at the captures in $work" -ForegroundColor Green
