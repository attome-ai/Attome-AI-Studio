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
    'key Home', 'wait 200', 'key Delete', 'wait 800'
    "shot $work\keys_deleted.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $w = Get-Workflow $run
      "after Home and Delete: $(Shape $w)"
      if (@($w.nodes.PSObject.Properties).Count -ne 3) { $failed = 'Home then Delete did not take exactly one node away' }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }
if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: nodes are chosen by key; look at the captures in $work" -ForegroundColor Green
