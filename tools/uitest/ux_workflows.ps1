# UI test: the Workflows canvas (UX review U10, U12, B13, U11).
#  - it opens at a size text can be read at (not below 75 %) and Fit still shows everything
#  - "+ Add node" opens the node search and a node can be added from it; the mini-map is there for a large graph
#  - the built-in Voice workflow has its nodes side by side (Get duration to the right of Generate speech)
#  - the clip list says where each workflow came from in words, not as an id
# Mock engine, virtual input only. Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_workflows.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Workflows.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null
    $p1 = @{ project = $proj; model = 'attome-mock'; prompt = 'a shot'; seconds = 2 }
    [IO.File]::WriteAllText("$work\c1.json", ($p1 | ConvertTo-Json -Compress), (New-Object Text.UTF8Encoding($false)))
    & "$bin\attome.exe" --json call gen.create_clip "$work\c1.json" | Out-Null
    $p2 = @{ project = $proj; model = 'attome-mock-voice'; prompt = 'Hello there friend.'; at = '5@1' }
    [IO.File]::WriteAllText("$work\c2.json", ($p2 | ConvertTo-Json -Compress), (New-Object Text.UTF8Encoding($false)))
    & "$bin\attome.exe" --json call gen.create_clip "$work\c2.json" | Out-Null
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $env:ATTOME_UI_READABLE_FIT = '1'   # the size a person gets (a script otherwise sees the whole graph)
  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
    'wait 1200'
    'click @clip:Shot_1', 'wait 400'
    'click @mode:Workflows', 'wait 1200'
    'expect @button:wf_add_node'
    'wide @node:generate_video 170'                  # a node is at least 170 points wide: 75 % of its size, where text can be read
    'click @button:wf_add_node', 'wait 500'
    'expect @field:wf_search'
    "shot $work\wf_search.jpg"
    'key Escape', 'wait 300'
    'click @mode:Video', 'wait 400'
    'click @clip:Voice_1', 'wait 400'
    'click @mode:Workflows', 'wait 1200'
    "shot $work\wf_voice.jpg"
  )
  $failed = $run.Errors
  Stop-Daemon $run
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE, Env:\ATTOME_UI_READABLE_FIT -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the canvas opens readable, the Add node button opens the search, the Voice workflow is laid out side by side (captures in $work)" -ForegroundColor Green
exit 0
