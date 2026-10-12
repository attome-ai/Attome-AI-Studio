# UI test: the Cast panel. The rail has Cast; a character is made by name, given how it looks, a picture, a voice and an attribute, and
# is then in the project; it is opened again and changed; a place is added. Virtual input only (uitest.psm1). Look at the captures too.
#   .\tools\uitest\cast.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Cast.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
$ep = "\\.\pipe\attome-uitest-cast-$PID"

$env:ATTOME_ENDPOINT = $ep
try { & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

# a picture and a sound to give the character: a drawn face, a second of tone
Add-Type -AssemblyName System.Drawing
$picture = Join-Path $work 'face.png'
$bmp = New-Object Drawing.Bitmap 256, 256
$g = [Drawing.Graphics]::FromImage($bmp)
$g.Clear([Drawing.Color]::FromArgb(40, 60, 90))
$g.FillEllipse([Drawing.Brushes]::Bisque, 58, 40, 140, 176)
$g.FillEllipse([Drawing.Brushes]::Black, 96, 104, 16, 16); $g.FillEllipse([Drawing.Brushes]::Black, 146, 104, 16, 16)
$g.Dispose(); $bmp.Save($picture, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
$voice = Join-Path $work 'voice.wav'
$rate = 16000; $n = $rate
$ms = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter $ms
$w.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $w.Write([int](36 + $n * 2)); $w.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
$w.Write([int]16); $w.Write([int16]1); $w.Write([int16]1); $w.Write([int]$rate); $w.Write([int]($rate * 2)); $w.Write([int16]2); $w.Write([int16]16)
$w.Write([Text.Encoding]::ASCII.GetBytes('data')); $w.Write([int]($n * 2))
for ($i = 0; $i -lt $n; $i++) { $w.Write([int16](8000 * [Math]::Sin(2 * [Math]::PI * 220 * $i / $rate))) }
[IO.File]::WriteAllBytes($voice, $ms.ToArray()); $w.Dispose()

function Get-Cast($run) {
  $p = Join-Path $work 'list.json'
  [IO.File]::WriteAllText($p, (@{ project = $proj } | ConvertTo-Json -Compress), (New-Object Text.UTF8Encoding($false)))
  @((Invoke-Attome $run --json call character.list $p | ConvertFrom-Json).result.cast)
}

# 1. a character by name, with how it looks; then a picture, a voice and an attribute
$env:ATTOME_EDITOR_PICK_CAST_PICTURE = $picture; $env:ATTOME_EDITOR_PICK_CAST_VOICE = $voice
try {
  $steps = @('wait 1200', 'click @rail:Cast', 'wait 400', "shot $work\empty.jpg",
             'click @button:cast_new_character', 'wait 300', 'click @field:cast_name', 'type Mio', 'click @field:cast_look',
             'type a young woman with a silver bob and a yellow raincoat', 'wait 200', "shot $work\typed.jpg",
             'click @button:cast_save', 'wait 700', 'expect @cast:Mio',
             'click @button:cast_add_picture', 'wait 900', 'expect @castpicture:1',
             'click @button:cast_voice', 'wait 700',
             'click @field:cast_attr_name', 'type alive', 'click @field:cast_attr_value', 'type no', 'click @button:cast_attr_add', 'wait 200',
             'click @button:cast_save', 'wait 900', 'expect @castattribute:alive', "shot $work\mio.jpg",
             'click @button:cast_new_place', 'wait 300', 'click @field:cast_name', 'type Loft', 'click @button:cast_save', 'wait 700',
             'expect @cast:Loft', "shot $work\two.jpg")
  $run = Invoke-EditorScript -Project $proj -Endpoint $ep -TimeoutSeconds 240 -Script $steps
} finally { Remove-Item Env:\ATTOME_EDITOR_PICK_CAST_PICTURE, Env:\ATTOME_EDITOR_PICK_CAST_VOICE -ErrorAction SilentlyContinue }
$failed = $run.Errors
if (-not $failed) {
  $cast = @(Get-Cast $run)
  "the cast: $(($cast | ForEach-Object { $_.name + ' (' + $_.kind + ')' }) -join ', ')"
  $mio = $cast | Where-Object { $_.name -eq 'Mio' }
  $loft = $cast | Where-Object { $_.name -eq 'Loft' }
  if (-not $mio) { $failed = 'Mio is not in the project' }
  elseif ($mio.description -ne 'a young woman with a silver bob and a yellow raincoat') { $failed = "her look was kept as: $($mio.description)" }
  elseif (@($mio.references).Count -ne 1 -or $mio.references[0].path -ne 'cast/face.png') { $failed = 'her picture was not kept in the project' }
  elseif ($mio.voice -ne 'cast/voice.wav') { $failed = "her voice was kept as: $($mio.voice)" }
  elseif ($mio.attributes.alive -ne $false) { $failed = 'the attribute alive is not no' }
  elseif (-not (Test-Path (Join-Path $proj 'cast\face.png'))) { $failed = 'the picture file is not in the project folder' }
  elseif (-not $loft -or $loft.kind -ne 'location') { $failed = 'the place was not added as a location' }
}

# 2. opened again: the attribute changes, the picture is taken out by its cross, the place is deleted
if (-not $failed) {
  $steps = @('wait 1200', 'click @rail:Cast', 'wait 400', 'click @cast:Mio', 'wait 400', 'click @castattribute:alive', 'key End', 'key Backspace', 'key Backspace', 'type yes', 'wait 200',
             'click @button:cast_save', 'wait 700', "shot $work\changed.jpg",
             'click @cast:Loft', 'wait 300', 'click @button:cast_delete', 'wait 700', "shot $work\deleted.jpg")
  $run2 = Invoke-EditorScript -Project $proj -Endpoint $ep -TimeoutSeconds 240 -Script $steps
  $failed = $run2.Errors
  if (-not $failed) {
    $cast = @(Get-Cast $run)
    if ($cast.Count -ne 1) { $failed = "expected 1 entry after the delete, found $($cast.Count)" }
    elseif ($cast[0].attributes.alive -ne $true) { $failed = 'the attribute alive was not changed to yes' }
  }
}
Stop-Daemon $run
if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the Cast panel makes, shows, changes and deletes entries; look at the captures in $work" -ForegroundColor Green
