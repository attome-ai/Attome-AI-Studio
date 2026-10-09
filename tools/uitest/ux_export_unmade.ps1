# UI test: an export with generated clips that are not made yet (UX review 5, V1). The export sheet names them, says what their
# place lacks, offers "Generate them first" and calls its button "Export anyway"; Generate them first makes them (on the mock
# engine), and the sheet opened again says nothing of them. Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_export_unmade.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Unmade.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null
    $info = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data
    $seq = $info.sequences[0].id
    @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"First","timing":{"record_in":"0","duration":"2","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":$(Get-ShotInstance 'a'),"inputs":{"prompt":"A robot walks","seed":3}}}},
 {"op":"add","path":"`$new:v1/clips/`$new:b","value":{"name":"Second","timing":{"record_in":"2","duration":"2","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":$(Get-ShotInstance 'b'),"inputs":{"prompt":"It rains","seed":4}}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
    & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'the setup patch was refused' }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script @(
    'wait 1200'
    'click @button:export', 'wait 800'
    'click @field:export_path', 'key A ctrl', "type $work\out.mp4", 'key Enter', 'wait 300'
    'expect @button:export_generate_first'
    "shot $work\export_warns.jpg"
    'click @button:export_generate_first', 'wait 4000'   # the sheet closes; the mock engine makes both
    'click @button:export', 'wait 800'
    'absent @button:export_generate_first'
    "shot $work\export_after_generating.jpg"
    'click @button:export_cancel', 'wait 300'
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $clips = (Invoke-Attome $run --json gen status $proj | ConvertFrom-Json).result.clips
      "status: $(($clips | ForEach-Object { "$($_.name)=$($_.state)" }) -join ', ')"
      if (@($clips | Where-Object { $_.state -ne 'clean' }).Count -ne 0) { $failed = 'Generate them first did not make both clips' }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the export sheet names the clips not made yet and makes them first on request (captures in $work)" -ForegroundColor Green
exit 0
