# UI test: the Text panel's ready-made styles (UX review 5, V6). Besides Title, Lower third and Caption, the panel has styles that set a
# look and a motion together: Bold pop (yellow, outlined, pops in), Neon (a glow), Typewriter (types itself on a dark box), Headline (on a
# white box, slides in) and Subtitle (a soft shadow, a caption). A click on each adds its clip with those content fields. Virtual input
# only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_text_presets.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Presets.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try { & "$bin\attome.exe" new $proj --rate 30 --canvas 1080x1920 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

# Each style's clip, by name: its content.
function Styled($run) {
  $out = @{}
  foreach ($t in @(Get-Tracks $run)) { foreach ($c in @($t.clip_list)) { $out[$c.name] = (Get-Object $run $c.id).content } }
  $out
}

$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'click @rail:Text', 'wait 500'
  "shot $work\text_panel.jpg"
  'click @style:Bold_pop', 'wait 900'
  "shot $work\bold_pop.jpg"
  'click @style:Neon', 'wait 900'
  "shot $work\neon.jpg"
  'click @style:Typewriter', 'wait 900'
  'click @style:Headline', 'wait 900'
  "shot $work\headline.jpg"
  'click @tab:Captions', 'wait 300', 'click @style:Subtitle', 'wait 900'
  "shot $work\subtitle.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $s = Styled $run
    "styles added: $(($s.Keys | Sort-Object) -join ', ')"
    if ($s['Bold pop'].color -ne '#ffe600' -or -not $s['Bold pop'].outline -or $s['Bold pop'].animate_in.style -ne 'pop') { $failed = "Bold pop is $($s['Bold pop'] | ConvertTo-Json -Compress -Depth 4)" }
    elseif ($s['Neon'].shadow.color -ne '#39ff88' -or $s['Neon'].animate_in.style -ne 'fade') { $failed = "Neon is $($s['Neon'] | ConvertTo-Json -Compress -Depth 4)" }
    elseif (-not $s['Typewriter'].background -or $s['Typewriter'].animate_in.style -ne 'typewriter') { $failed = "Typewriter is $($s['Typewriter'] | ConvertTo-Json -Compress -Depth 4)" }
    elseif ($s['Headline'].background.color -ne '#ffffff' -or $s['Headline'].animate_in.style -ne 'slide') { $failed = "Headline is $($s['Headline'] | ConvertTo-Json -Compress -Depth 4)" }
    elseif (-not $s['Subtitle'].shadow -or $s['Subtitle'].animate_in) { $failed = "Subtitle is $($s['Subtitle'] | ConvertTo-Json -Compress -Depth 4)" }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result.problems | ConvertTo-Json -Compress)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the Text panel's styles add their look and motion (captures in $work)" -ForegroundColor Green
exit 0
