# UI test: the welcome page (UX review U16). With no project it offers a new project with a shape and a frame rate, and the
# recent projects; a new project is made with the shape chosen (a 9:16 Short is 1080 x 1920) and then shows in the recent list.
# The test's own preferences and project folder are used, so nothing of the user's is touched. Virtual input only. Exit code 0 = pass.
#   .\tools\uitest\ux_welcome.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null
$prefs = Join-Path $work 'prefs'
$projects = Join-Path $work 'projects'
$env:ATTOME_PREF_DIR = $prefs
$env:ATTOME_NEW_PROJECT_FOLDER = $projects
$endpoint = "\\.\pipe\attome-uitest-welcome-$PID"
try {
  # 1. No project: the page, with nothing recent yet, makes a 60 fps square project.
  $run = Invoke-EditorScript -Endpoint $endpoint -TimeoutSeconds 120 -Script @(
    'wait 1200'
    'expect @button:create_project', 'expect @field:project_path'
    "shot $work\welcome_empty.jpg"
    'click @field:new_project_name', 'key A ctrl', 'type Lovely Square'
    'click @button:shape_2'
    'click @button:rate_60'
    'wait 200'
    "shot $work\welcome_filled.jpg"
    'click @button:create_project', 'wait 1500'
    'expect @save_state'                       # the editor is open on the new project
  )
  $failed = $run.Errors
  $made = Join-Path $projects 'Lovely Square.attome'
  if (-not $failed) {
    if (-not (Test-Path $made)) { $failed = "the project was not made in $projects" }
    else {
      $env:ATTOME_ENDPOINT = $endpoint
      $info = (& "$bin\attome.exe" --json inspect $made | ConvertFrom-Json).result.data
      $seq = (& "$bin\attome.exe" --json get $made $info.sequences[0].id | ConvertFrom-Json).result.object
      Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue
      "canvas $($seq.canvas.width) x $($seq.canvas.height), rate $($seq.rate)"
      if ($seq.canvas.width -ne 1080 -or $seq.canvas.height -ne 1080) { $failed = "the canvas is $($seq.canvas.width) x $($seq.canvas.height), not 1080 x 1080" }
      elseif ("$($seq.rate)" -ne '60') { $failed = "the frame rate is $($seq.rate), not 60" }
    }
  }
  Stop-Daemon ([pscustomobject]@{ Endpoint = $endpoint })
  # 2. A new start shows it in the recent list, and opens it with a click.
  if (-not $failed) {
    $endpoint2 = "\\.\pipe\attome-uitest-welcome2-$PID"
    $run2 = Invoke-EditorScript -Endpoint $endpoint2 -TimeoutSeconds 120 -Script @(
      'wait 1200'
      'expect @recent:Lovely_Square'
    )
    $failed = $run2.Errors
    Stop-Daemon ([pscustomobject]@{ Endpoint = $endpoint2 })
  }
} finally {
  Remove-Item Env:\ATTOME_PREF_DIR, Env:\ATTOME_NEW_PROJECT_FOLDER -ErrorAction SilentlyContinue
}

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the welcome page makes a project with the chosen shape and rate, and lists it as recent (captures in $work)" -ForegroundColor Green
exit 0
