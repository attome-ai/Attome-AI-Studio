# UI test: the font, the slant, the alignment and the line spacing of a text (UX review U6). The Text card has a Font list (the
# families fonts.list names), Italic, Left / Center / Right and a Lines slider; they are saved as content.font, italic, align and
# line_spacing, and the picture shows them (captures). Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_text_font.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Font.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
$fontName = $null
try {
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  $fonts = (& "$bin\attome.exe" --json call fonts.list | ConvertFrom-Json).result.fonts
  "fonts: $(@($fonts).Count)"
  if (@($fonts).Count -lt 2) { throw 'fonts.list names no fonts' }
  $fontName = if ($fonts -contains 'Consolas') { 'Consolas' } else { $fonts[0] }
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_text'; text = "Hello there`nsecond"; name = 'Hello'; at = '0s'; duration = '3s' }) } | ConvertTo-Json -Depth 5 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Content($run) { $t = (@(Get-Tracks $run) | Where-Object { @($_.clip_list).Count -ge 1 } | Select-Object -First 1); (Get-Object $run $t.clip_list[0].id).content }
function Has($o, [string]$name) { $o.PSObject.Properties.Name -contains $name }

$failed = $null
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'click @clip:Hello', 'wait 500'
  'expect @field:text_font', 'expect @check:italic', 'expect @button:text_align_left', 'expect @slider:text_line_spacing'
  "shot $work\font_before.jpg"
  'click @field:text_font', 'wait 400'
  "click @font:$fontName", 'wait 800'
  'click @check:italic', 'wait 600'
  'click @button:text_align_right', 'wait 600'
  'slide @slider:text_line_spacing 0.8', 'wait 700'
  "shot $work\font_after.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $c = Content $run
    "content: font=$($c.font) italic=$($c.italic) align=$($c.align) line_spacing=$($c.line_spacing)"
    if ($c.font -ne $fontName) { $failed = "the font is '$($c.font)', not '$fontName'" }
    elseif ($c.italic -ne $true) { $failed = 'Italic did not set content.italic' }
    elseif ($c.align -ne 'right') { $failed = "the alignment is '$($c.align)', not right" }
    elseif (-not (Has $c 'line_spacing') -or $c.line_spacing -lt 1.3) { $failed = "the line spacing is '$($c.line_spacing)', not wide after the slider" }
  }
  if (-not $failed) { # back to the defaults: Center and Default take the fields away; one undo brings the last back
    $back = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('click @clip:Hello', 'wait 400', 'click @button:text_align_center', 'wait 600', 'click @field:text_font', 'wait 300', 'click @font:Default', 'wait 600')
    $failed = $back.Errors
    if (-not $failed) {
      $c = Content $run
      if ((Has $c 'align') -or (Has $c 'font')) { $failed = 'Center and Default left a field on the text' }
    }
  }
  if (-not $failed) { # a click on a card's title folds it away and brings it back
    $fold = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('click @clip:Hello', 'wait 400', 'expect @check:italic', 'click @card:Text', 'wait 400', 'absent @check:italic', "shot $work\card_folded.jpg", 'click @card:Text', 'wait 400', 'expect @check:italic')
    $failed = $fold.Errors
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result.problems | ConvertTo-Json -Compress)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a text's font, slant, alignment and line spacing are set on its card, saved in its content, and taken away again (captures in $work)" -ForegroundColor Green
exit 0

