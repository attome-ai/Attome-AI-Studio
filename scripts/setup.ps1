#Requires -Version 5.1
<#
.SYNOPSIS
  One-command setup for Attome on Windows: installs missing tools, fetches
  dependencies, and builds the project.

.PARAMETER CheckOnly  Only report what is installed. Changes nothing.
.PARAMETER Yes        Do not ask before installing missing tools.
.PARAMETER DepsOnly   Install tools and dependencies, but do not build.
.PARAMETER Config     release (default) or debug.
#>
[CmdletBinding()]
param(
  [switch]$CheckOnly,
  [switch]$Yes,
  [switch]$DepsOnly,
  [ValidateSet('release', 'debug')][string]$Config = 'release'
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Deps = Join-Path $Root '.deps'
$VcpkgDir = Join-Path $Deps 'vcpkg'

function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "  [ok]      $m" -ForegroundColor Green }
function Miss($m) { Write-Host "  [missing] $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host "error: $m" -ForegroundColor Red; exit 1 }

function Refresh-Path {
  $m = [Environment]::GetEnvironmentVariable('Path', 'Machine')
  $u = [Environment]::GetEnvironmentVariable('Path', 'User')
  $env:Path = "$m;$u"
}

function Get-Ver([string]$text) {
  if ($text -match '(\d+\.\d+(\.\d+)?)') { return [version]$Matches[1] }
  return $null
}

function Find-Vs {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) { return $null }
  $p = & $vswhere -latest -products * -version '[17.10,)' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if ($p) { return $p }
  return $null
}

# ---- what we need ---------------------------------------------------------
# Name, test script (returns $true when fine), winget id (or $null), extra args
$tools = @(
  @{ Name = 'Git';                 Test = { Get-Command git -ErrorAction SilentlyContinue };                      Id = 'Git.Git' },
  @{ Name = 'CMake 3.28 or newer'; Test = { $c = Get-Command cmake -ErrorAction SilentlyContinue; $c -and ((Get-Ver (& cmake --version | Select-Object -First 1)) -ge [version]'3.28') }; Id = 'Kitware.CMake' },
  @{ Name = 'Ninja';               Test = { Get-Command ninja -ErrorAction SilentlyContinue };                    Id = 'Ninja-build.Ninja' },
  @{ Name = 'Python 3.11 or newer';Test = { $c = Get-Command python -ErrorAction SilentlyContinue; $c -and ((Get-Ver (& python --version)) -ge [version]'3.11') }; Id = 'Python.Python.3.12' },
  @{ Name = 'uv (Python packages)';Test = { Get-Command uv -ErrorAction SilentlyContinue };                       Id = 'astral-sh.uv' },
  @{ Name = 'Vulkan SDK';          Test = { $env:VULKAN_SDK -and (Test-Path $env:VULKAN_SDK) };                   Id = 'KhronosGroup.VulkanSDK' },
  @{ Name = 'Visual Studio 2022 C++ build tools (17.10+)'; Test = { Find-Vs };                                   Id = 'Microsoft.VisualStudio.2022.BuildTools';
     Extra = @('--override', '--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended') }
)

Step 'Checking your machine'
if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
  Write-Host 'winget was not found. It ships with Windows 10 (1809+) and 11 as "App Installer" in the Microsoft Store.' -ForegroundColor Yellow
}
$missing = @()
foreach ($t in $tools) {
  $found = $false
  try { $found = [bool](& $t.Test) } catch { $found = $false }
  if ($found) { Ok $t.Name } else { Miss $t.Name; $missing += $t }
}
if (Test-Path (Join-Path $VcpkgDir 'vcpkg.exe')) { Ok 'vcpkg (in .deps)' } else { Miss 'vcpkg (will be cloned into .deps)' }

if ($CheckOnly) {
  if ($missing.Count -eq 0) { Write-Host 'Everything needed is installed.' -ForegroundColor Green; exit 0 }
  Write-Host "$($missing.Count) item(s) missing. Run setup.cmd to install them." -ForegroundColor Yellow
  exit 1
}

# ---- install missing tools -----------------------------------------------
if ($missing.Count -gt 0) {
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Die 'Install winget (App Installer) or install the missing tools by hand, then run again.' }
  if (-not $Yes) {
    $names = ($missing | ForEach-Object { $_.Name }) -join ', '
    $a = Read-Host "Install with winget: $names ? [Y/n]"
    if ($a -match '^(n|no)$') { Die 'Cancelled.' }
  }
  foreach ($t in $missing) {
    Step "Installing $($t.Name)"
    $args = @('install', '--id', $t.Id, '-e', '--accept-package-agreements', '--accept-source-agreements', '--silent')
    if ($t.Extra) { $args += $t.Extra }
    & winget @args
    # winget returns non-zero when a package is already current; verify instead of trusting the code
    Refresh-Path
  }
  # Vulkan SDK sets VULKAN_SDK for new shells only
  if (-not $env:VULKAN_SDK) {
    $v = [Environment]::GetEnvironmentVariable('VULKAN_SDK', 'Machine')
    if (-not $v) { $v = [Environment]::GetEnvironmentVariable('VULKAN_SDK', 'User') }
    if ($v) { $env:VULKAN_SDK = $v }
  }
}

# ---- vcpkg (kept inside the project folder, nothing global) ---------------
New-Item -ItemType Directory -Force -Path $Deps | Out-Null
if (-not (Test-Path (Join-Path $VcpkgDir '.git'))) {
  Step 'Fetching vcpkg'
  & git clone https://github.com/microsoft/vcpkg.git $VcpkgDir
  if ($LASTEXITCODE -ne 0) { Die 'Could not clone vcpkg.' }
}
if (-not (Test-Path (Join-Path $VcpkgDir 'vcpkg.exe'))) {
  Step 'Bootstrapping vcpkg'
  & (Join-Path $VcpkgDir 'bootstrap-vcpkg.bat') -disableMetrics
  if ($LASTEXITCODE -ne 0) { Die 'vcpkg bootstrap failed.' }
}
$env:VCPKG_ROOT = $VcpkgDir
$env:VCPKG_DISABLE_METRICS = '1'

# ---- Python environment for the AI host ----------------------------------
$py = Join-Path $Root 'ai-host\pyproject.toml'
if ((Test-Path $py) -and (Get-Command uv -ErrorAction SilentlyContinue)) {
  Step 'Setting up the Python environment'
  Push-Location (Join-Path $Root 'ai-host')
  try { & uv sync } finally { Pop-Location }
}

# ---- build ----------------------------------------------------------------
if ($DepsOnly) { Write-Host 'Tools and dependencies are ready.' -ForegroundColor Green; exit 0 }
if (-not (Test-Path (Join-Path $Root 'CMakeLists.txt'))) {
  Write-Host ''
  Write-Host 'Your toolchain is ready. This checkout has no engine sources to build yet.' -ForegroundColor Green
  Write-Host 'Run update.cmd later to pull new code and build it.'
  exit 0
}

$vs = Find-Vs
if (-not $vs) { Die 'Visual Studio C++ build tools were not found after install. Open a new terminal and run setup.cmd again.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$preset = "win-msvc-$Config"
Step "Building ($preset)"
$cmd = "`"$vcvars`" >nul && cd /d `"$Root`" && cmake --preset $preset && cmake --build --preset $preset"
& cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) { Die 'The build failed. The messages above say why.' }

Write-Host ''
Write-Host 'Done. Attome is built.' -ForegroundColor Green
Write-Host "Try:  build\$preset\bin\attome.exe --version"
