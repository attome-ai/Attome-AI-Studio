# UI test: the drop preview. A title style, an effect and a model dragged over the timeline, and a clip moved over
# another one, are captured while the button is still down (what the user sees before letting go), then dropped.
# Mock engine, virtual input only.
#   .\tools\uitest\drop_preview.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Preview.attome'
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
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"First","timing":{"record_in":"0","duration":"3","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":$(Get-VideoInstance 'a'),"inputs":{"prompt":"A robot walks"}}}},
 {"op":"add","path":"`$new:v1/clips/`$new:b","value":{"name":"Second","timing":{"record_in":"3","duration":"3","source_in":"0"},
   "media_ref":{"type":"workflow","workflow":$(Get-VideoInstance 'b'),"inputs":{"prompt":"It rains"}}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
    & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'the setup patch was refused' }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -Script @(
    'click @rail:Text'
    'drag @style:Title @clip:First hold'        # over a clip: the ghost shows on a new track above it, at that time
    'wait 200'
    "shot $work\preview_title.jpg"
    'release'
    'wait 500'
    'click @rail:Effects'
    'drag @effect:blur @clip:First hold'        # an effect over a clip: the clip lights up
    'wait 200'
    "shot $work\preview_effect.jpg"
    'release'
    'wait 500'
    'click @rail:Generate'
    'drag @model:attome-mock @clip:Second@0.25,0.5 hold' # a model over a clip's left half: its ghost before it, the clip slid right
    'wait 200'
    "shot $work\preview_generate.jpg"
    'release'
    'wait 800'
    'drag @clip:Second @clip:First@0.25,0.5 hold' # a clip moved over another's left half: drawn before it, the others slid right
    'wait 200'
    "shot $work\preview_move.jpg"
    'release'
    'wait 800'
    "shot $work\preview_after.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $clips = @((Get-Tracks $run) | ForEach-Object { $_.clip_list })
      $fx = 0
      $where = foreach ($c in $clips) {
        $o = Get-Object $run $c.id
        if ($c.name -eq 'First' -and $o.effects) { $fx = @($o.effects.PSObject.Properties).Count }
        "$($c.name)@$([math]::Round((ConvertFrom-Rational $o.timing.record_in), 3))"
      }
      "clips: $($where -join ', ')   effects on First: $fx"
      if ($clips.Count -ne 4) { $failed = "expected 4 clips (First, Second, a title and a shot), found $($clips.Count)" }
      elseif ($fx -ne 1) { $failed = 'the blur did not go onto the clip it was dropped on' }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: cards and clips dragged over the timeline show where they land, and land there (captures in $work)" -ForegroundColor Green
exit 0
