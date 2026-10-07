# UI test: ProRes and DNxHR in the Export sheet (made by the FFmpeg of this computer).
#  - with no FFmpeg the sheet lists the three formats and says where ProRes and DNxHR will appear
#  - with one (ATTOME_TEST_FFMPEG, or a Krita / Kdenlive copy on this computer) both are offered, with their profiles, and the export writes a .mov
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_export_prores.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Prores.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$work\*.mov" -Force -ErrorAction SilentlyContinue

# An FFmpeg to use: the test's own, else one that other programs on this computer carry.
$ffmpeg = $env:ATTOME_TEST_FFMPEG
if (-not $ffmpeg) {
  foreach ($c in 'C:\Program Files\Krita (x64)\bin\ffmpeg.exe', 'C:\Program Files\kdenlive\bin\ffmpeg.exe') { if (Test-Path $c) { $ffmpeg = $c; break } }
}

$env:ATTOME_SETTINGS = Join-Path $work 'settings.json' # never the user's own settings
Remove-Item $env:ATTOME_SETTINGS -Force -ErrorAction SilentlyContinue
$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 3 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $call = @{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\a.mp4"; at = '0s' }) }
  [IO.File]::WriteAllText("$work\call.json", ($call | ConvertTo-Json -Depth 5 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$failed = @()
# 1. No FFmpeg anywhere the daemon looks: only the three formats, and the hint.
$savedPath = $env:PATH; $env:PATH = ''; $env:ATTOME_FFMPEG = ''
try {
  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script @(
    'wait 1500'
    'click @button:export', 'wait 600'
    'expect @button:export_format_video', 'expect @button:export_format_picture'
    'absent @button:export_format_prores', 'absent @button:export_format_dnxhr'
    "shot $work\no_ffmpeg.jpg"
    'key Escape', 'wait 300'
  )
  $failed += $run.Errors
} finally { $env:PATH = $savedPath }

# 2. With one: both formats, the profiles, and a .mov comes out.
if ($ffmpeg) {
  $env:ATTOME_FFMPEG = $ffmpeg
  $out = Join-Path $work 'film.mov'
  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 300 -Script @(
    'wait 1500'
    'click @button:export', 'wait 600'
    'expect @button:export_format_prores', 'expect @button:export_format_dnxhr'
    'click @button:export_format_prores', 'wait 300'
    'expect @button:export_profile_HQ', 'expect @button:export_profile_Proxy', 'absent @button:export_quality_High'
    'click @button:export_profile_Proxy', 'wait 200'
    "shot $work\prores_sheet.jpg"
    'click @field:export_path', 'key A ctrl', "type $out"
    'click @button:export_res_720p'
    'click @button:export_start', 'wait 3500'
    'expect @button:export_play'
    'click @button:export_close', 'wait 300'
    'click @button:export', 'wait 600'
    'click @button:export_format_dnxhr', 'wait 300'
    'expect @button:export_profile_HQX'
    "shot $work\dnxhr_sheet.jpg"
    'key Escape', 'wait 300'
  )
  $failed += $run.Errors
  if (-not (Test-Path $out)) { $failed += "no .mov was written at $out" }
  elseif ((Get-Item $out).Length -lt 10000) { $failed += "the .mov is too small: $((Get-Item $out).Length) bytes" }
  $env:ATTOME_FFMPEG = $null
} else {
  Write-Output 'NOTE: no FFmpeg on this computer (set ATTOME_TEST_FFMPEG); only the no-FFmpeg half ran.'
}
$env:ATTOME_SETTINGS = $null

$failed = @($failed | Where-Object { $_ })
if ($failed.Count) { $failed | ForEach-Object { Write-Output "FAIL: $_" }; exit 1 }
Write-Output "PASS: ProRes and DNxHR are offered only with an FFmpeg, with profiles, and the export writes a .mov (captures in $work)"
exit 0
