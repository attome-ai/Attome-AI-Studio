# UI test: two generative clips on the mock engine, the second starting on the last frame of the first. The Generate
# button on the second clip's card makes both (the first is needed and not made yet), the Takes arrive by themselves, and
# the Monitor shows the generated picture. No model and no GPU: the mock engine draws a coloured picture.
# Virtual input only (uitest.psm1). Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\generate.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Generate.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null
    $info = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data
    $prj = $info.id; $seq = $info.sequences[0].id
    @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"First","timing":{"record_in":"0","duration":"2","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":$(Get-ShotInstance 'a'),"inputs":{"prompt":"A robot walks","seed":3}}}},
 {"op":"add","path":"`$new:v1/clips/`$new:b","value":{"name":"Second","timing":{"record_in":"2","duration":"2","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":$(Get-ShotInstance 'b' -StartFrom '$new:a'),"inputs":{"prompt":"It starts to rain"}}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
    & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'the setup patch was refused' }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -Script @(
    'click @clip:Second'
    'wait 500'
    'expect @field:prompt'
    "shot $work\generate_before.jpg"   # both clips with the amber stripe: not made yet
    'click @button:gen_run'            # the second clip: the first is made too, because the second starts from it
    'wait 300'
    "shot $work\generate_running.jpg"  # the card shows the step that is running, and Stop
    'expect @button:gen_run'           # back when the run has ended; now it says "New take"
    'wait 1500'                        # the Takes arrive with the next look at the project
    "shot $work\generate_after.jpg"    # the Monitor shows the generated picture; the card says up to date
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $clips = (Invoke-Attome $run --json gen status $proj | ConvertFrom-Json).result.clips
      "status: $(($clips | ForEach-Object { "$($_.name)=$($_.state) takes=$($_.takes)" }) -join ', ')"
      if (@($clips | Where-Object { $_.state -ne 'clean' -or $_.takes -ne 1 }).Count -ne 0) { $failed = 'both clips are not clean with one Take each' }
    }
    if (-not $failed) { # another Take from the command line: the next seed
      $again = Invoke-Attome $run --json gen run $proj --clip ($clips[0].clip) --new-take | ConvertFrom-Json
      $after = (Invoke-Attome $run --json gen status $proj | ConvertFrom-Json).result.clips
      "after a new take of First: $(($after | ForEach-Object { "$($_.name)=$($_.state) takes=$($_.takes)" }) -join ', ')"
      if (-not $again.ok) { $failed = 'attome gen run --new-take failed' }
      elseif ($after[0].takes -ne 2 -or $after[0].state -ne 'clean') { $failed = 'the first clip did not get a second Take' }
      elseif ($after[1].state -ne 'dirty') { $failed = 'the second clip is not out of date after the first changed' }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Generate from the clip's card made both clips, and a new Take left the clip after it out of date (captures in $work)" -ForegroundColor Green
exit 0
