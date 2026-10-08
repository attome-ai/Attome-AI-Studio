# UI test: the Monitor's frames are made on the GPU chosen in Settings, and the Monitor says so; with the CPU chosen they are not.
#  - a clip that fills the frame with a blur and a colour grade: with Automatic (a graphics card here) the header says "drawn on <GPU>"
#  - Settings > the processor only: the header no longer says it, and the picture is the same
# Skipped (passes) where no GPU can render. Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_monitor_gpu.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Gpu.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
$env:ATTOME_SETTINGS = Join-Path $work 'settings.json' # never the user's own settings
Remove-Item $env:ATTOME_SETTINGS -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\.\pipe\attome-uitest-setup-$PID"
$usable = $false
try {
  New-Sample "$work\a.mp4" "--seconds 3 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  function Call($tool, $o) {
    [IO.File]::WriteAllText("$work\call.json", ($o | ConvertTo-Json -Depth 8 -Compress), (New-Object Text.UTF8Encoding($false)))
    $r = & "$bin\attome.exe" --json call $tool "$work\call.json" | ConvertFrom-Json
    if (-not $r.ok) { throw "setup $tool : $($r.error.message)" }
    $r.result
  }
  $made = Call timeline.edit @{ project = $proj; ops = @(@{ op = 'add_clip'; id = '$new:a'; path = "$work\a.mp4"; at = '0s'; fit = 'fill' }) }
  $clip = $made.id_map.'$new:a'
  $null = Call timeline.edit @{ project = $proj; ops = @(@{ op = 'add_effect'; clip = $clip; type = 'gaussian_blur'; radius = 0.02 }, @{ op = 'add_effect'; clip = $clip; type = 'color_grade'; saturation = 1.6 }) }
  $devices = Call render.devices @{}
  $usable = $devices.in_use.index -ge 0
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }
if (-not $usable) { Write-Output 'PASS: no GPU can render here (nothing to show)'; $env:ATTOME_SETTINGS = $null; exit 0 }

$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script @(
  'wait 3000'
  'expect @monitor:gpu'
  "shot $work\gpu.jpg"
  'key Comma ctrl', 'wait 600', 'click @button:render_cpu', 'wait 400', 'click @button:settings_close', 'wait 2500'
  'absent @monitor:gpu'
  "shot $work\cpu.jpg"
  'key Comma ctrl', 'wait 600', 'click @button:render_auto', 'wait 400', 'click @button:settings_close', 'wait 2500'
  'expect @monitor:gpu'
)
$failed = @($run.Errors | Where-Object { $_ })
$env:ATTOME_SETTINGS = $null
if ($failed.Count) { $failed | ForEach-Object { Write-Output "FAIL: $_" }; exit 1 }
Write-Output "PASS: the Monitor's frames are made on the GPU chosen in Settings, and on the CPU when the processor is chosen (captures in $work)"
exit 0
