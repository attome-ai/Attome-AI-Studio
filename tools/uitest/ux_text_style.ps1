# UI test: the look of a text (UX review U6). The Text card has ready-made styles and sliders for the outline, the shadow and the box;
# they are saved as the clip's content.outline, content.shadow and content.background, undo takes a style back, and the picture
# shows them (checked by a capture). Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_text_style.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Style.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_text'; text = 'Hello'; name = 'Hello'; at = '0s'; duration = '3s' }) } | ConvertTo-Json -Depth 5 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Content($run) { $t = (@(Get-Tracks $run) | Where-Object { @($_.clip_list).Count -ge 1 } | Select-Object -First 1); (Get-Object $run $t.clip_list[0].id).content }
function Has($o, [string]$name) { $o.PSObject.Properties.Name -contains $name }

$failed = $null
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'click @clip:Hello', 'wait 500'
  'expect @button:text_style_pop'
  "shot $work\style_plain.jpg"
  'click @button:text_style_pop', 'wait 900'
  "shot $work\style_pop.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $c = Content $run
    "after Pop: outline=$(Has $c 'outline') shadow=$(Has $c 'shadow') background=$(Has $c 'background')"
    if (-not (Has $c 'outline') -or -not (Has $c 'shadow')) { $failed = 'the Pop style did not set an outline and a shadow' }
    elseif (Has $c 'background') { $failed = 'the Pop style set a box' }
    elseif ($c.shadow.opacity -le 0 -or $c.outline.width -le 0) { $failed = 'the outline or the shadow is off' }
  }
  if (-not $failed) { # Box: replaces the look; the sliders set a value
    $box = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('click @clip:Hello', 'wait 400', 'click @button:text_style_box', 'wait 700', "shot $work\style_box.jpg")
    $failed = $box.Errors
    if (-not $failed) {
      $c = Content $run
      "after Box: outline=$(Has $c 'outline') shadow=$(Has $c 'shadow') background=$(Has $c 'background')"
      if (-not (Has $c 'background') -or (Has $c 'outline') -or (Has $c 'shadow')) { $failed = 'the Box style did not replace the look with a box' }
    }
  }
  if (-not $failed) {
    $sl = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('click @clip:Hello', 'wait 400', 'slide @slider:text_box 0.4', 'wait 700')
    $failed = $sl.Errors
    if (-not $failed) {
      $c = Content $run
      "box opacity after the slider: $($c.background.opacity)"
      if ([math]::Abs($c.background.opacity - 0.4) -gt 0.08) { $failed = "the box slider left an opacity of $($c.background.opacity), not about 0.4" }
    }
  }
  if (-not $failed) { # Plain takes the look away; one undo brings the last one back
    $pl = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('click @clip:Hello', 'wait 400', 'click @button:text_style_plain', 'wait 700')
    $failed = $pl.Errors
    if (-not $failed) {
      $c = Content $run
      if ((Has $c 'outline') -or (Has $c 'shadow') -or (Has $c 'background')) { $failed = 'Plain left a look on the text' }
    }
  }
  if (-not $failed) {
    $un = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl', 'wait 800')
    $failed = $un.Errors
    if (-not $failed -and -not (Has (Content $run) 'background')) { $failed = 'undo did not bring the box back' }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result.problems | ConvertTo-Json -Compress)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: ready-made text styles and the outline, shadow and box sliders set the clip's look, and undo takes a style back (captures in $work)" -ForegroundColor Green
exit 0
