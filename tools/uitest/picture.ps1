# UI test: importing a transparent PNG makes a 5 s picture clip, shows it on the timeline and in the Media panel, and
# leaves the canvas alone (a logo is no canvas). Virtual input only (uitest.psm1). Exit code 0 = pass.
#   .\tools\uitest\picture.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP 'attome-uitest'
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Picture.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# A 300 x 200 logo: an orange disc on a transparent background.
$png = Join-Path $work 'logo.png'
$bmp = New-Object System.Drawing.Bitmap 300, 200, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'
$g.Clear([System.Drawing.Color]::Transparent)
$g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 120, 20))), 75, 25, 150, 150)
$g.Dispose()
$bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try { & "$bin\attome.exe" new $proj --rate 30 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$run = Invoke-EditorScript -Project $proj -Import $png -Script @(
  'wait 500'
  'expect @clip:logo'
  "shot $work\picture.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $clips = @((Get-Tracks $run) | ForEach-Object { $_.clip_list } | Where-Object { $_ } | ForEach-Object { Get-Object $run $_.id })
    $pic = $clips | Where-Object { $_.media_ref.type -eq 'image' } | Select-Object -First 1
    $canvas = (Invoke-Attome $run --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].canvas
    "clips: $($clips.Count), picture: $($pic.media_ref | ConvertTo-Json -Compress), duration $($pic.timing.duration), canvas $($canvas.width) x $($canvas.height)"
    if (-not $pic) { $failed = 'importing a PNG should make a picture clip (media_ref.type "image")' }
    elseif ((ConvertFrom-Rational $pic.timing.duration) -ne 5) { $failed = "a picture clip should last 5 s (got $($pic.timing.duration))" }
    elseif ($clips.Count -ne 1) { $failed = 'a picture has no sound, so it should not get a linked sound clip' }
    elseif ($canvas -and [int]$canvas.width -ne 1920) { $failed = "importing a logo should not change the canvas (now $($canvas.width) x $($canvas.height))" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a transparent PNG became a 5 s picture clip, the canvas unchanged (captures in $work)" -ForegroundColor Green
exit 0
