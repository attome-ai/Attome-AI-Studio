# UI test: the Length of a generative clip is set on its own card. Made longer, it is as long as what it asks for, and
# the clip after it slides right instead of being overlapped; made shorter, nothing else moves. Mock engine.
#   .\tools\uitest\clip_length.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Length.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try { & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  function Layout($run) {
    @((Get-Tracks $run)[0].clip_list | ForEach-Object {
      $o = Get-Object $run $_.id
      [pscustomobject]@{ Name = $_.name; At = [math]::Round((ConvertFrom-Rational $o.timing.record_in), 3); Len = [math]::Round((ConvertFrom-Rational $o.timing.duration), 3); }
    } | Sort-Object At)
  }
  function Show($l) { ($l | ForEach-Object { "$($_.Name)@$($_.At)+$($_.Len)" }) -join ', ' }

  # Two 5-second shots, touching. The mock model makes 0.1 to 15 s: the slider at 0.5 sets the Duration (what the Clip node gives) to 7.5 s.
  $run = Invoke-EditorScript -Project $proj -Script @(
    'click @rail:Generate'
    'click @model:attome-mock'
    'wait 500'
    'click @model:attome-mock'
    'wait 500'
    'click @clip:Shot_1'
    'wait 300'
    'slide @slider:gen_length 0.5'
    'wait 600'
    "shot $work\length_longer.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $l = Layout $run; "longer: $(Show $l)"
      if ($l[0].Len -ne 7.5) { $failed = 'Shot 1 does not last 7.5 s' }
      elseif ($l[1].At -ne 7.5) { $failed = "Shot 2 is at $($l[1].At) s, not slid to 7.5 s" }
    }
    if (-not $failed) {
      $short = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('click @clip:Shot_1', 'wait 300', 'slide @slider:gen_length 0.1', 'wait 600')
      $failed = $short.Errors
      if (-not $failed) {
        $l = Layout $run; "shorter: $(Show $l)"
        if ($l[0].Len -ge 7.5) { $failed = 'Shot 1 did not get shorter' }
        elseif ($l[1].At -ne 7.5) { $failed = 'Shot 2 moved when Shot 1 got shorter' }
      }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a clip's Length is set on its card, and a longer clip pushes the next one along (captures in $work)" -ForegroundColor Green
exit 0
