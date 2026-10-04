# UI test: a model card from the Generate panel dragged onto a clip of the timeline makes a generative clip after it
# (never an overlap), and a clip of the timeline dragged onto another one lands after that one, not back where it was. Mock engine, virtual input only.
#   .\tools\uitest\generate_drop.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Drop.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\.\pipe\attome-uitest-setup-$PID"
  try {
    & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null
    $info = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data
    $prj = $info.id; $seq = $info.sequences[0].id
    @"
{"ops":[
 {"op":"add","path":"$prj/workflows/`$new:shot","value":{"name":"Shot","builtin":"shot:attome-mock",
   "nodes":{"`$new:gen":{"kind":"attome.generate_video","model":"attome-mock"}},
   "exposed":{"inputs":{"prompt":["`$new:gen","prompt"],"seed":["`$new:gen","seed"],"seconds":["`$new:gen","seconds"],"width":["`$new:gen","width"],"height":["`$new:gen","height"]},
              "outputs":{"video":["`$new:gen","video"],"audio":["`$new:gen","audio"],"last_frame":["`$new:gen","last_frame"]}}}},
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"First","timing":{"record_in":"0","duration":"32017/3200","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":"`$new:shot","inputs":{"prompt":"A robot walks"}}}},
 {"op":"add","path":"`$new:v1/clips/`$new:b","value":{"name":"Second","timing":{"record_in":"32017/3200","duration":"5","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":"`$new:shot","inputs":{"prompt":"It rains"}}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
    & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'the setup patch was refused' }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -Script @(
    'click @rail:Generate'
    'wait 300'
    "shot $work\drop_before.jpg"
    'drag @model:attome-mock @clip:First'   # released on top of the clip
    'wait 800'
    "shot $work\drop_after.jpg"
    'drag @clip:First @clip:Second'          # a clip of the timeline released on top of another one
    'wait 800'
    "shot $work\move_after.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $clips = @((Get-Tracks $run) | ForEach-Object { $_.clip_list })
      "clips: $(($clips | ForEach-Object { $_.name }) -join ', ')"
      if ($clips.Count -ne 3) { $failed = "expected 3 clips after the drop, found $($clips.Count)" }
      else {
        $at = @{}; foreach ($c in $clips) { $at[$c.name] = ConvertFrom-Rational (Get-Object $run $c.id).timing.record_in }
        $at.GetEnumerator() | ForEach-Object { "$($_.Key) at $($_.Value) s" }
        if ($at['First'] -lt 15.0) { $failed = "First went back to $($at['First']) s instead of the free space after Second (15.005 s)" }
      }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a card dropped on a clip made a second clip after it (captures in $work)" -ForegroundColor Green
exit 0


