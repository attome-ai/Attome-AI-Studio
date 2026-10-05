# UI test: a model card from the Generate panel dragged onto a clip of the timeline makes a generative clip after it
# (the clip's right half) or before it (its left half); the clips in the way slide right, also around a clip that ends
# between frames (10.005 s). Mock engine, virtual input only.
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
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
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
    'drag @model:attome-mock @clip:First@0.75,0.5'   # the right half of First: after it, and Second slides right
    'wait 800'
    "shot $work\drop_after.jpg"
    'drag @model:attome-mock @clip:First@0.25,0.5'   # the left half: before it, and everything slides right
    'wait 800'
    "shot $work\drop_before_first.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $clips = @((Get-Tracks $run) | ForEach-Object { $_.clip_list } | ForEach-Object {
        $o = Get-Object $run $_.id
        $at = ConvertFrom-Rational $o.timing.record_in
        [pscustomobject]@{ Name = $_.name; At = $at; End = $at + (ConvertFrom-Rational $o.timing.duration) }
      } | Sort-Object At)
      "clips: $(($clips | ForEach-Object { "$($_.Name)@$([math]::Round($_.At, 3))" }) -join ', ')"
      if ($clips.Count -ne 4) { $failed = "expected 4 clips after the two drops, found $($clips.Count)" }
      elseif (($clips.Name -join ',') -ne 'Shot 2,First,Shot 1,Second') { $failed = 'the clips are not in the order Shot 2, First, Shot 1, Second' }
      else {
        for ($i = 1; $i -lt $clips.Count; ++$i) {
          if ($clips[$i].At -lt $clips[$i - 1].End - 1e-9) { $failed = "$($clips[$i - 1].Name) and $($clips[$i].Name) overlap" }
          elseif ($clips[$i].At - $clips[$i - 1].End -gt 0.05) { $failed = "a gap opened between $($clips[$i - 1].Name) and $($clips[$i].Name)" }
        }
      }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a model dropped after and before a clip, the clips in the way sliding right (captures in $work)" -ForegroundColor Green
exit 0


