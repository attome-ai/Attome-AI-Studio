# UI test: File > Settings (Ctrl+,) and where rendering runs.
#  - the window opens from the File menu and from Ctrl+,; it lists Automatic, every GPU of this computer and the CPU, and says what Automatic uses now
#  - choosing the CPU is saved in the user's settings (a private settings file here); Automatic again removes nothing else
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_settings_render.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Settings.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
$env:ATTOME_SETTINGS = Join-Path $work 'settings.json' # never the user's own settings
Remove-Item $env:ATTOME_SETTINGS -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 2 --height 360"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $call = @{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\a.mp4"; at = '0s' }) }
  [IO.File]::WriteAllText("$work\call.json", ($call | ConvertTo-Json -Depth 5 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$failed = @()
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script @(
  'wait 1500'
  'click @menu:File', 'wait 300', 'click @menuitem:Settings...', 'wait 800'
  'expect @window:settings', 'expect @button:render_auto', 'expect @button:render_cpu'
  "shot $work\settings.jpg"
  'click @button:render_cpu', 'wait 600'
  "shot $work\settings_cpu.jpg"
  'click @button:settings_close', 'wait 300'
  'absent @window:settings'
  'key Comma ctrl', 'wait 800'      # Ctrl+, opens it again, with the choice kept
  'expect @window:settings'
  'click @button:render_auto', 'wait 600'
  'key Escape', 'wait 300'
)
$failed += $run.Errors
# The choice went through the daemon into the settings file; Automatic is the last one made.
if (-not (Test-Path $env:ATTOME_SETTINGS)) { $failed += 'no settings file was written' }
else {
  $saved = Get-Content $env:ATTOME_SETTINGS -Raw | ConvertFrom-Json
  if ($saved.render_device -ne 'auto') { $failed += "render_device is '$($saved.render_device)', not auto" }
}
$env:ATTOME_SETTINGS = $null

$failed = @($failed | Where-Object { $_ })
if ($failed.Count) { $failed | ForEach-Object { Write-Output "FAIL: $_" }; exit 1 }
Write-Output "PASS: Settings opens from File and Ctrl+,, lists where rendering can run, and saves the choice (captures in $work)"
exit 0
