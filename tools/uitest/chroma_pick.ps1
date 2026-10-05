# UI test: the chroma key's colour picker returns the hue of the pixel it is clicked on. A picture of two flat halves
# (green 30,200,60 = 130.6 degrees; blue 20,60,220 = 228.0 degrees) is keyed, and each half is picked in turn; the hue
# must match the colour's own to within 3 degrees. Virtual input only (uitest.psm1). Needs a build
# (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\chroma_pick.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'ChromaPick.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$png = Join-Path $work 'halves.png'
$bmp = New-Object System.Drawing.Bitmap 960, 540, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 30, 200, 60))), 0, 0, 480, 540)
$g.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 20, 60, 220))), 480, 0, 480, 540)
$g.Dispose()
$bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try { & "$bin\attome.exe" new $proj --rate 30 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Get-KeyHue($run) {
  $clip = (@(Get-Tracks $run) | ForEach-Object { $_.clip_list } | Where-Object { $_.name -eq 'halves' } | Select-Object -First 1)
  $e = @((Get-Object $run $clip.id).effects.PSObject.Properties | ForEach-Object { $_.Value })[0]
  [double]$e.params.hue
}

$failed = $null
$green = $null; $blue = $null
$run = Invoke-EditorScript -Project $proj -Import $png -Script @(
  'wait 800'
  'expect @clip:halves'
  'click @clip:halves'
  'wait 300'
  'click @rail:Effects'
  'drag @effect:key @clip:halves'
  'click @button:pick_key'
  'click @monitor@0.25,0.5'             # the green half
  'wait 800'
)
$failed = $run.Errors
try { if (-not $failed) { $green = Get-KeyHue $run } } finally { Stop-Daemon $run }
if (-not $failed) {
  $run = Invoke-EditorScript -Project $proj -Script @(
    'wait 800'
    'expect @clip:halves'
    'click @clip:halves'
    'wait 300'
    'expect @button:pick_key'
    'click @button:pick_key'
    'click @monitor@0.75,0.5'           # the blue half
    'wait 800'
    "shot $work\chroma_pick.jpg"
  )
  $failed = $run.Errors
  try { if (-not $failed) { $blue = Get-KeyHue $run } } finally { Stop-Daemon $run }
}
if (-not $failed) {
  "picked: green half -> $green (130.6), blue half -> $blue (228.0)"
  if ([math]::Abs($green - 130.6) -gt 3) { $failed = "the green half gave hue $green, expected 130.6" }
  elseif ([math]::Abs($blue - 228.0) -gt 3) { $failed = "the blue half gave hue $blue, expected 228.0" }
}

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the colour picker returns the clicked pixel's hue (captures in $work)" -ForegroundColor Green
exit 0
