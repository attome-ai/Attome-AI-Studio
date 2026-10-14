# UI test: pictures for what is not a person, and for niches. Cast: a new place takes pictures before it is saved (the button, or a dropped file) and
# they go in with it; a picture dropped on the Cast panel with nothing open makes a new entry from it; a dropped picture goes to the entry that is
# open. Niches: a niche of the user's keeps pictures (the button or a drop), the cross takes one out, and skill.get gives the agent their paths.
# Mock engine, virtual input only. Exit code 0 = pass.   .\tools\uitest\pictures_friendly.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Pics.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

function Call($tool, $o) {
  $f = Join-Path $work 'call.json'
  [IO.File]::WriteAllText($f, ($o | ConvertTo-Json -Depth 20 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --daemon never --json call $tool $f | ConvertFrom-Json
  if (-not $r.ok) { throw "$tool failed: $($r.error.message)" }
  $r.result
}

$colours = @{ loft = '200,120,60'; barn = '90,140,90'; street = '60,90,200'; look = '230,200,90' }
foreach ($n in $colours.Keys) { python -c "from PIL import Image; Image.new('RGB', (96, 64), ($($colours[$n]))).save(r'$work\$n.png')" }

$env:ATTOME_MOCK_ENGINE = '1'
$failed = $null
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try { & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $env:ATTOME_EDITOR_PICK_CAST_PICTURE = "$work\loft.png"
  $env:ATTOME_EDITOR_PICK_NICHE_PICTURE = "$work\look.png"
  try {
    $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
      'wait 1000'
      'click @rail:Cast', 'wait 500'
      'click @button:cast_new_place', 'wait 300'
      'click @field:cast_name', 'type Loft'
      'click @button:cast_add_picture', 'wait 700'          # before it is saved
      'expect @castpicture:1'
      "drop $work\barn.png", 'wait 700'                     # a dropped picture goes to the entry that is open
      'expect @castpicture:2'
      "shot $work\unsaved.jpg"
      'click @button:cast_save', 'wait 900'
      'expect @cast:Loft'
      'expect @castpicture:2'
      'click @button:pick_close', 'wait 400'                # nothing is open now
      "drop $work\street.png", 'wait 1000'                  # a picture on the empty panel makes an entry
      'expect @cast:street'
      "shot $work\dropped_entry.jpg"
      'click @rail:Niches', 'wait 500'
      'click @button:niche_new', 'wait 300', 'click @button:niche_new_empty', 'wait 400'
      'click @field:niche_name', 'key A ctrl', 'type Dusk', 'click @field:niche_text', 'type Warm and low.'
      'click @button:niche_add_picture', 'wait 700'         # before it is saved
      'expect @castpicture:1'
      'click @button:niche_save', 'wait 1000'
      "drop $work\loft.png", 'wait 900'                     # a niche that is saved keeps a dropped picture at once
      'expect @castpicture:2'
      "shot $work\niche.jpg"
    )
  } finally { Remove-Item Env:\ATTOME_EDITOR_PICK_CAST_PICTURE, Env:\ATTOME_EDITOR_PICK_NICHE_PICTURE -ErrorAction SilentlyContinue }
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $cast = @((Call character.list @{ project = $proj }).cast)
      "cast: $(($cast | ForEach-Object { $_.name + ':' + @($_.references).Count }) -join ', ')"
      $loft = $cast | Where-Object { $_.name -eq 'Loft' }
      $street = $cast | Where-Object { $_.name -eq 'street' }
      if (-not $loft -or @($loft.references).Count -ne 2) { $failed = 'Loft should have 2 pictures' }
      elseif ($loft.kind -ne 'location') { $failed = 'Loft should be a place' }
      elseif (-not $street -or @($street.references).Count -ne 1) { $failed = 'the dropped picture should have made the entry "street" with 1 picture' }
    }
    if (-not $failed) {
      $skills = @((Call skill.list @{ project = $proj }).skills | Where-Object { $_.source -eq 'project' })
      $dusk = $skills | Where-Object { $_.title -eq 'Dusk' }
      if (-not $dusk) { throw 'the niche Dusk was not saved' }
      $got = Call skill.get @{ id = $dusk.id; project = $proj }
      "niche pictures: $(@($got.images).Count)"
      if (@($got.images).Count -ne 2) { $failed = "the niche should have 2 pictures, it has $(@($got.images).Count)" }
      elseif (-not (Test-Path $got.images[0].path)) { $failed = 'the picture path skill.get gives is not a file' }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: places take pictures before they are saved or by a drop; a dropped picture makes an entry; a niche keeps pictures (captures in $work)" -ForegroundColor Green
exit 0
