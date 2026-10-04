<#
  UI test helpers for attome-editor on Windows (PowerShell 5.1).

  A test runs a separate editor instance with its own daemon and project and drives it with a script of virtual input
  (app\editor\uidriver.hpp): the editor feeds the input to its UI itself, so the system mouse and keyboard are never
  touched and the window never takes the focus. The computer stays usable while tests run.

  Scripts name widgets instead of pixels (click @button:add_dissolve, slide @slider:gain 0.5, click @clip:music.wav)
  and the editor scrolls a widget into view before using it, so tests survive layout changes.

    Import-Module .\tools\uitest\uitest.psm1
    $run = Invoke-EditorScript -Project C:\temp\T.attome -SelectFirstClip -Script @(
      'click @button:add_dissolve'
      'shot C:\temp\after.jpg'
    )
    if ($run.Errors) { ... }                               # the script's own failures ("uitest: line N: ...")
    Invoke-Attome $run get C:\temp\T.attome clp_...        # the CLI, pointed at this editor's daemon
    Stop-Daemon $run

  The user's own editor and projects (the default daemon endpoint) are never used.
#>

$script:Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Get-BinDir { Join-Path $script:Root 'build\win-msvc-release\bin' }

# The CLI against this editor's own daemon.
function Invoke-Attome {
  param($Editor, [Parameter(ValueFromRemainingArguments = $true)] $Args)
  $old = $env:ATTOME_ENDPOINT; $env:ATTOME_ENDPOINT = $Editor.Endpoint
  try { & (Join-Path (Get-BinDir) 'attome.exe') @Args } finally { $env:ATTOME_ENDPOINT = $old }
}

# Runs the editor on $Project with a UI script and waits for it to finish. -Endpoint reuses the daemon of an earlier
# run, so several scripts can work on one project. Returns { Endpoint, Project, ExitCode, Errors }.
function Invoke-EditorScript {
  param([Parameter(Mandatory)][string]$Project, [Parameter(Mandatory)][string[]]$Script, [string[]]$Import = @(),
        [switch]$SelectFirstClip, [string]$Endpoint, [int]$TimeoutSeconds = 90)
  if (-not $Endpoint) { $Endpoint = "\\.\pipe\attome-uitest-$PID-$([guid]::NewGuid().ToString('N').Substring(0, 6))" }
  $tag = [guid]::NewGuid().ToString('N').Substring(0, 8)
  $file = Join-Path $env:TEMP "attome-uitest-$tag.txt"
  $errFile = Join-Path $env:TEMP "attome-uitest-$tag.err"
  [IO.File]::WriteAllLines($file, $Script, (New-Object Text.UTF8Encoding $false))
  $saved = @{ ATTOME_ENDPOINT = $env:ATTOME_ENDPOINT; ATTOME_EDITOR_SCRIPT = $env:ATTOME_EDITOR_SCRIPT; ATTOME_EDITOR_SELECT = $env:ATTOME_EDITOR_SELECT }
  $env:ATTOME_ENDPOINT = $Endpoint
  $env:ATTOME_EDITOR_SCRIPT = $file
  if ($SelectFirstClip) { $env:ATTOME_EDITOR_SELECT = '1' } else { $env:ATTOME_EDITOR_SELECT = $null }
  $editorArgs = (@($Project) + $Import | ForEach-Object { "`"$_`"" }) -join ' ' # media files are imported on start
  try {
    $p = Start-Process (Join-Path (Get-BinDir) 'attome-editor.exe') -ArgumentList $editorArgs -PassThru -RedirectStandardError $errFile
    $null = $p.Handle # keeps the exit code readable
  } finally {
    foreach ($k in $saved.Keys) { Set-Item "Env:\$k" $saved[$k] -ErrorAction SilentlyContinue; if (-not $saved[$k]) { Remove-Item "Env:\$k" -ErrorAction SilentlyContinue } }
  }
  $errors = ''
  if (-not $p.WaitForExit($TimeoutSeconds * 1000)) { $p | Stop-Process -Force; $errors = "the script did not finish in $TimeoutSeconds s" }
  $p.WaitForExit()
  $errors = (($errors, (Get-Content $errFile -Raw -ErrorAction SilentlyContinue)) | Where-Object { $_ }) -join "`n"
  Remove-Item $file, $errFile -ErrorAction SilentlyContinue
  [pscustomobject]@{ Endpoint = $Endpoint; Project = $Project; ExitCode = $p.ExitCode; Errors = ($errors -split "`n" | Where-Object { $_ -match 'uitest|did not finish' }) -join "`n" }
}

# `attome sample <path> <args>` (a synthetic clip), encoded once and copied afterwards: the same clip is asked for by many
# tests and encoding it takes more than half a second.
function New-Sample([string]$Path, [string]$SampleArgs) {
  $cache = Join-Path $env:TEMP 'attome-uitest-cache'
  New-Item -ItemType Directory -Force $cache | Out-Null
  $key = ($SampleArgs -replace '[^0-9a-z]+', '_').Trim('_')
  $cached = Join-Path $cache ("$key" + [IO.Path]::GetExtension($Path))
  if (-not (Test-Path $cached)) {
    $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-sample-$PID"
    try { & (Join-Path (Get-BinDir) 'attome.exe') sample $cached @($SampleArgs -split ' ' | Where-Object { $_ }) | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }
  }
  Copy-Item $cached $Path -Force
}

function Stop-Daemon($Run) {
  $ErrorActionPreference = 'Continue' # "no daemon running" is fine
  Invoke-Attome $Run daemon stop 2>&1 | Out-Null
}

# The tracks of the first sequence, with their clip lists.
function Get-Tracks($Run) {
  (Invoke-Attome $Run --json inspect $Run.Project --level tracks | ConvertFrom-Json).result.data.sequences[0].tracks
}

function Get-Object($Run, [string]$Id) { (Invoke-Attome $Run --json get $Run.Project $Id | ConvertFrom-Json).result.object }

# Seconds from a stored rational time such as "7/2".
function ConvertFrom-Rational([string]$T) {
  if ($T -match '/') { $p = $T -split '/'; [double]$p[0] / [double]$p[1] } else { [double]$T }
}

Export-ModuleMember -Function New-Sample, Invoke-EditorScript, Invoke-Attome, Stop-Daemon, Get-Tracks, Get-Object, ConvertFrom-Rational
