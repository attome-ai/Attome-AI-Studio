# UI test: effects, text styles and workflows kept in the library are used in another project. Project A: the card of a clip's blur has "Add to the
# library" (the text style and the workflow are kept with library.add_item, as an agent would). Project B (another editor, the same library):
# Library > Effects, a double click puts the blur on the selected clip; Text, the style on the selected text; Workflows, a template of the project.
# The library is the test's own folder. Mock engine, virtual input only. Exit code 0 = pass.   .\tools\uitest\library_kinds.ps1

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
    New-Sample "$work\talk.mp4" "--seconds 3 --height 360"
    foreach ($p in $a, $b) { & "$bin\attome.exe" new $p --rate 24 --canvas 640x352 | Out-Null }
    $ra = Call timeline.edit @{ project = $a; ops = @(
        @{ op = 'add_clip'; id = '$new:v'; path = "$work\talk.mp4"; name = 'talk'; at = '0s' },
        @{ op = 'add_effect'; target = '$new:v'; type = 'gaussian_blur'; radius = 0.03 },
        @{ op = 'add_text'; id = '$new:t'; text = 'Big'; name = 'Head'; at = '0s'; duration = '2s'; size = 0.12; color = '#ffcc00' }) }
    $text = $ra.id_map.'$new:t'
    $shot = (Call gen.create_clip @{ project = $a; prompt = 'A talk'; model = 'attome-mock'; seconds = 3 }).clip
    Call library.add_item @{ project = $a; kind = 'text_style'; source = $text; name = 'Look' } | Out-Null
    Call library.add_item @{ project = $a; kind = 'workflow'; source = $shot; name = 'Flow' } | Out-Null
    Call timeline.edit @{ project = $b; ops = @(
        @{ op = 'add_clip'; path = "$work\talk.mp4"; name = 'talk'; at = '0s' },
        @{ op = 'add_text'; text = 'Small'; name = 'Head'; at = '0s'; duration = '2s' }) } | Out-Null
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $runA = Invoke-EditorScript -Project $a -TimeoutSeconds 200 -Script @(
    'wait 1000'
    'click @clip:talk', 'wait 600'
    'expect @button:keep_blur'
    "shot $work\effect_card.jpg"
    'click @button:keep_blur', 'wait 1200'
    'click @rail:Library', 'wait 700'
    'click @tab:Effects', 'wait 400'
    'expect @library:Blur'
    "shot $work\library_effects.jpg"
    'click @tab:Workflows', 'wait 400'
    'expect @library:Flow'
    'click @tab:Text', 'wait 400'
    'expect @library:Look'
    "shot $work\library_text.jpg"
  )
  $failed = $runA.Errors
  Stop-Daemon $runA
  if (-not $failed) {
    $items = @(Get-ChildItem (Join-Path $lib 'clips') -Directory)
    if ($items.Count -ne 3) { $failed = "the library has $($items.Count) items, not 3" }
  }
  if (-not $failed) {
    $runB = Invoke-EditorScript -Project $b -TimeoutSeconds 240 -Script @(
      'wait 1000'
      'click @clip:talk', 'wait 500'
      'click @rail:Library', 'wait 700'
      'click @tab:Effects', 'wait 400'
      'dblclick @library:Blur', 'wait 1200'            # on the selected clip
      'click @rail:Media', 'wait 300'
      'click @clip:Head', 'wait 500'
      'click @rail:Library', 'wait 700'
      'click @tab:Text', 'wait 400'
      'dblclick @library:Look', 'wait 1200'            # on the selected text
      'click @tab:Workflows', 'wait 400'
      'dblclick @library:Flow', 'wait 1200'            # a template of this project
      "shot $work\project_b.jpg"
    )
    $failed = $runB.Errors
    Stop-Daemon $runB
    if (-not $failed) {
      $doc = Get-Content (Join-Path $b 'project.json') -Raw | ConvertFrom-Json
      $text = $null; $video = $null
      $doc | ConvertTo-Json -Depth 40 | Out-Null
      $json = Get-Content (Join-Path $b 'project.json') -Raw
      if ($json -notmatch 'attome\.gaussian_blur') { $failed = 'the blur is not on the clip of project B' }
      elseif ($json -notmatch '#ffcc00') { $failed = 'the text of project B did not take the colour of the style' }
      elseif (-not $doc.workflows -or @($doc.workflows.PSObject.Properties).Count -ne 1) { $failed = 'project B should have one template' }
      elseif ($json -match '"text": "Big"') { $failed = 'the style brought its words along' }
    }
  }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: an effect, a text style and a workflow kept in the library are used in another project (captures in $work)" -ForegroundColor Green
exit 0
