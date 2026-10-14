# UI test: who is in a generated clip is one line of tags in its Workflow card. A tag has an x while the pointer is on it (it takes that person out of the
# clip); the + at the end lists the cast entries that are not in the clip and adds the one picked. Mock engine, virtual input only.
#   .\tools\uitest\cast_chips.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Chips.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

function Call($tool, $o) {
  $f = Join-Path $work 'call.json'
  [IO.File]::WriteAllText($f, ($o | ConvertTo-Json -Depth 20 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --daemon never --json call $tool $f | ConvertFrom-Json
  if (-not $r.ok) { throw "$tool failed: $($r.error.message)" }
  $r.result
}

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null
    foreach ($n in 'Mira', 'Elias', 'Zed') { Call character.create @{ project = $proj; name = $n; description = "$n, a person" } | Out-Null }
    $clip = (Call gen.create_clip @{ project = $proj; prompt = 'A talk'; model = 'attome-mock'; seconds = 3 }).clip
    Call character.use @{ project = $proj; clip = $clip; character = 'Mira'; voice = $false } | Out-Null
    Call character.use @{ project = $proj; clip = $clip; character = 'Elias'; voice = $false } | Out-Null
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script @(
    'wait 1000'
    'click @clip:Shot_1', 'wait 500'
    'expect @cast_chip:Mira'
    'expect @cast_chip:Elias'
    "shot $work\tags.jpg"
    'click @button:gen_cast_add', 'wait 400'
    'expect @cast_option:Zed'
    "shot $work\add_menu.jpg"
    'click @cast_option:Zed', 'wait 700'
    'expect @cast_chip:Zed'
    'click @cast_chip_x:Mira', 'wait 700'                  # the x of a tag takes that person out of the clip
    'absent @cast_chip:Mira'
    "shot $work\after.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $tracks = @(Get-Tracks $run)
      $c = Get-Object $run $tracks[0].clip_list[0].id
      $in = @($c.media_ref.workflow.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq 'attome.character' } | ForEach-Object { $_.Value.character })
      $cast = (Call character.list @{ project = $proj }).cast
      $names = @($in | ForEach-Object { $id = $_; ($cast | Where-Object { $_.id -eq $id }).name }) | Sort-Object
      "in the clip: $($names -join ', ')"
      if (($names -join ',') -ne 'Elias,Zed') { $failed = "the clip should have Elias and Zed, it has: $($names -join ', ')" }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the cast of a clip is a line of tags: + adds from the cast, the x of a tag takes it out (captures in $work)" -ForegroundColor Green
exit 0
