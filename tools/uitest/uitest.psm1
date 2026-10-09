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
  param([string]$Project = '', [Parameter(Mandatory)][string[]]$Script, [string[]]$Import = @(),
        [switch]$SelectFirstClip, [string]$Endpoint, [int]$TimeoutSeconds = 90)
  if (-not $Endpoint) { $Endpoint = "\\.\pipe\attome-uitest-$PID-$([guid]::NewGuid().ToString('N').Substring(0, 6))" }
  $tag = [guid]::NewGuid().ToString('N').Substring(0, 8)
  $file = Join-Path $env:TEMP "attome-uitest-$tag.txt"
  $errFile = Join-Path $env:TEMP "attome-uitest-$tag.err"
  [IO.File]::WriteAllLines($file, $Script, (New-Object Text.UTF8Encoding $false))
  $saved = @{ ATTOME_ENDPOINT = $env:ATTOME_ENDPOINT; ATTOME_EDITOR_SCRIPT = $env:ATTOME_EDITOR_SCRIPT; ATTOME_EDITOR_SELECT = $env:ATTOME_EDITOR_SELECT; ATTOME_PREF_DIR = $env:ATTOME_PREF_DIR
              ATTOME_LIBRARY_DIR = $env:ATTOME_LIBRARY_DIR }
  if (-not $env:ATTOME_PREF_DIR) { $env:ATTOME_PREF_DIR = Join-Path $env:TEMP "attome-uitest-prefs-$tag" } # the user's own preferences are never used by a test
  if (-not $env:ATTOME_LIBRARY_DIR) { $env:ATTOME_LIBRARY_DIR = Join-Path $env:TEMP "attome-uitest-library-$tag" } # nor the user's clip library
  $env:ATTOME_ENDPOINT = $Endpoint
  $env:ATTOME_EDITOR_SCRIPT = $file
  if ($SelectFirstClip) { $env:ATTOME_EDITOR_SELECT = '1' } else { $env:ATTOME_EDITOR_SELECT = $null }
  $editorArgs = (@($(if ($Project) { $Project })) + $Import | Where-Object { $_ } | ForEach-Object { "`"$_`"" }) -join ' ' # media files are imported on start
  try {
    $start = @{ FilePath = (Join-Path (Get-BinDir) 'attome-editor.exe'); PassThru = $true; RedirectStandardError = $errFile }
    if ($editorArgs) { $start.ArgumentList = $editorArgs } # no project: the welcome page
    $p = Start-Process @start
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
  $cache = Join-Path $env:TEMP 'attome-uitest-cache2'
  New-Item -ItemType Directory -Force $cache | Out-Null
  $key = ($SampleArgs -replace '[^0-9a-z]+', '_').Trim('_')
  $name = "$key" + [IO.Path]::GetExtension($Path)
  $cached = Join-Path $cache $name
  if (-not (Test-Path $cached)) {
    # The picture's colour is a hash of the file name as `attome sample` is given it. Made with a relative name, it is the same wherever
    # %TEMP% is, so a test that looks for a colour in it (chroma_key) does not depend on the folder it runs from. It is made in a folder of its
    # own and moved into the cache whole: tests run side by side, and one must never copy a sample that another is still writing.
    $stage = Join-Path $cache ('stage-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force $stage | Out-Null
    $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-sample-$PID"
    Push-Location $stage
    try { & (Join-Path (Get-BinDir) 'attome.exe') sample $name @($SampleArgs -split ' ' | Where-Object { $_ }) | Out-Null } finally { Pop-Location; Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }
    try { Move-Item (Join-Path $stage $name) $cached -ErrorAction Stop } catch { } # another test was first: its file is the same
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    if (-not (Test-Path $cached)) { throw "the sample $name could not be made" }
  }
  Copy-Item $cached $Path -Force
}

# A generative clip's own workflow, as JSON text to put in a patch (`"workflow": <this>`): the "Shot" of a model as one
# "generate video" node. $X keeps the placeholders of two clips apart in one patch. -Exposed names the inputs the clip sets.
function Get-VideoInstance([string]$X = '', [string]$Model = 'attome-mock', [string]$Settings = '', [string]$Inputs = '',
                           [string[]]$Exposed = @('prompt', 'seed', 'seconds', 'width', 'height')) {
  $types = @{ prompt = 'text'; seed = 'integer'; seconds = 'number'; width = 'integer'; height = 'integer'; start_image = 'image' }
  $in = $Exposed | ForEach-Object { """$_"":{""type"":""$($types[$_])"",""order"":$([array]::IndexOf($Exposed, $_)),""to"":[[""`$new:gen$X"",""$_""]]$(if ($_ -eq 'prompt') { ',"required":true' })}" }
  $node = """kind"":""attome.generate_video"",""model"":""$Model"""
  if ($Settings) { $node += ",""settings"":$Settings" }
  if ($Inputs) { $node += ",""inputs"":$Inputs" }
  "{""name"":""Shot"",""source"":""test"",""nodes"":{""`$new:gen$X"":{$node}},""exposed"":{""inputs"":{$($in -join ',')},""outputs"":{""video"":{""from"":[""`$new:gen$X"",""video""]},""audio"":{""from"":[""`$new:gen$X"",""audio""]}},""primary"":""video""}}"
}

# The same as three blocks (encode the prompt, sample, decode), with a start picture and a seed to set.
# -StartFrom names the clip it starts on the last frame of ("previous" or a clip ID, "$new:a" in a patch): a Clip Reference node and
# a Get Frame node feed the start picture, and the Exposed Input of it is left unlinked.
function Get-ShotInstance([string]$X = '', [string]$Model = 'attome-mock', [int]$Seconds = 2, [int]$Width = 640, [int]$Height = 352,
                          [string[]]$Exposed = @('prompt', 'start_image', 'seed'), [string]$StartFrom = '') {
  $where = @{ prompt = 'enc'; start_image = 'smp'; seed = 'smp' }
  $types = @{ prompt = 'text'; seed = 'integer'; start_image = 'image' }
  $in = $Exposed | ForEach-Object {
    $to = if ($_ -eq 'start_image' -and $StartFrom) { '' } else { ",""to"":[[""`$new:$($where[$_])$X"",""$_""]]" }
    """$_"":{""type"":""$($types[$_])"",""order"":$([array]::IndexOf($Exposed, $_))$to}"
  }
  $from_nodes = ''; $from_links = ''
  if ($StartFrom) {
    $from_nodes = ",""`$new:ref$X"":{""kind"":""attome.clip_reference"",""settings"":{""clip"":""$StartFrom""}},""`$new:prev$X"":{""kind"":""attome.get_frame"",""settings"":{""frame"":""last""}}"
    $from_links = ",""`$new:l4$X"":{""from"":[""`$new:ref$X"",""video""],""to"":[""`$new:prev$X"",""video""]},""`$new:l5$X"":{""from"":[""`$new:prev$X"",""image""],""to"":[""`$new:smp$X"",""start_image""]}"
  }
  "{""name"":""Shot"",""source"":""test"",""nodes"":{" +
    """`$new:enc$X"":{""kind"":""attome.encode_prompt"",""model"":""$Model""}," +
    """`$new:smp$X"":{""kind"":""attome.sample"",""model"":""$Model"",""settings"":{""steps"":4},""inputs"":{""seconds"":$Seconds,""width"":$Width,""height"":$Height}}," +
    """`$new:dec$X"":{""kind"":""attome.decode"",""model"":""$Model""}," +
    """`$new:frm$X"":{""kind"":""attome.get_frame"",""settings"":{""frame"":""last""}}$from_nodes}," +
    """links"":{""`$new:l1$X"":{""from"":[""`$new:enc$X"",""conditioning""],""to"":[""`$new:smp$X"",""conditioning""]},""`$new:l2$X"":{""from"":[""`$new:smp$X"",""latent""],""to"":[""`$new:dec$X"",""latent""]}," +
    """`$new:l3$X"":{""from"":[""`$new:dec$X"",""video""],""to"":[""`$new:frm$X"",""video""]}$from_links}," +
    """exposed"":{""inputs"":{$($in -join ',')},""outputs"":{""video"":{""from"":[""`$new:dec$X"",""video""]},""last_frame"":{""from"":[""`$new:frm$X"",""image""]}},""primary"":""video""}}"
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

Export-ModuleMember -Function Get-VideoInstance, Get-ShotInstance, New-Sample, Invoke-EditorScript, Invoke-Attome, Stop-Daemon, Get-Tracks, Get-Object, ConvertFrom-Rational
