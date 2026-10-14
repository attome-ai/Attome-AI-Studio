# UI test: a cast entry is kept in the library from its card's menu and used in another project. Project A: right-click the card of Mira, "Add to the
# library"; project B (another editor, the same library): Library > Cast, a double click puts Mira in its cast, with her picture, and a second one
# keeps both under different names. The library is the test's own folder. Mock engine, virtual input only. Exit code 0 = pass.
#   .\tools\uitest\library_cast.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$a = Join-Path $work 'A.attome'; $b = Join-Path $work 'B.attome'
$lib = Join-Path $work 'library'
foreach ($p in $a, $b, $lib) { Remove-Item $p -Recurse -Force -ErrorAction SilentlyContinue }
$env:ATTOME_LIBRARY_DIR = $lib

function Call($tool, $o) {
  $f = Join-Path $work 'call.json'
  [IO.File]::WriteAllText($f, ($o | ConvertTo-Json -Depth 20 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --daemon never --json call $tool $f | ConvertFrom-Json
  if (-not $r.ok) { throw "$tool failed: $($r.error.message)" }
  $r.result
}

$env:ATTOME_MOCK_ENGINE = '1'
$failed = $null
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    python -c "from PIL import Image; Image.new('RGB', (64, 64), (200, 90, 60)).save(r'$work\mira.png')"
    & "$bin\attome.exe" new $a --rate 24 --canvas 640x352 | Out-Null
    & "$bin\attome.exe" new $b --rate 24 --canvas 640x352 | Out-Null
    Call character.create @{ project = $a; name = 'Mira'; description = 'a tall woman'; references = @("$work\mira.png") } | Out-Null
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $runA = Invoke-EditorScript -Project $a -TimeoutSeconds 200 -Script @(
    'wait 1000'
    'click @rail:Cast', 'wait 600'
    'expect @cast:Mira'
    'rclick @cast:Mira', 'wait 400'
    'click @menuitem:Add_to_the_library', 'wait 1200'
    'click @rail:Library', 'wait 700'
    'click @tab:Cast', 'wait 500'
    'expect @library:Mira'
    "shot $work\library_a.jpg"
  )
  $failed = $runA.Errors
  Stop-Daemon $runA
  if (-not $failed) {
    $items = @(Get-ChildItem (Join-Path $lib 'clips') -Directory)
    if ($items.Count -ne 1) { $failed = "the library has $($items.Count) items, not 1" }
    elseif (-not (Test-Path (Join-Path $items[0].FullName 'thumb.jpg')) -and -not (Get-ChildItem (Join-Path $items[0].FullName 'media') -Filter '*mira.png')) { $failed = 'the picture was not kept' }
    else { Remove-Item "$work\mira.png" -Force }   # the item has its own copy
  }
  if (-not $failed) {
    $runB = Invoke-EditorScript -Project $b -TimeoutSeconds 200 -Script @(
      'wait 1000'
      'click @rail:Library', 'wait 700'
      'click @tab:Cast', 'wait 400'
      'expect @library:Mira'
      'dblclick @library:Mira', 'wait 1200'
      'dblclick @library:Mira', 'wait 1200'     # a second time: the cast already has Mira
      'click @rail:Cast', 'wait 700'
      'expect @cast:Mira'
      "shot $work\cast_b.jpg"
    )
    $failed = $runB.Errors
    Stop-Daemon $runB
    if (-not $failed) {
      $cast = (Call character.list @{ project = $b }).cast
      "cast of B: $(($cast | ForEach-Object { $_.name }) -join ', ')"
      if (@($cast).Count -ne 2) { $failed = "the cast of B should have 2 entries, it has $(@($cast).Count)" }
      elseif (-not ($cast | Where-Object { $_.name -eq 'Mira' -and $_.description -eq 'a tall woman' })) { $failed = 'Mira did not come with her description' }
      elseif (-not (Test-Path (Join-Path $b ($cast[0].references[0].path)))) { $failed = 'her picture was not copied into the project' }
    }
  }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a cast card keeps Mira in the library; another project adds her, with her picture and look (captures in $work)" -ForegroundColor Green
exit 0
