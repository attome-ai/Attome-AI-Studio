# UI test: a clip whose workflow decides its length. On the Workflow card, "Length by" is changed from Me to the workflow (a Get Duration
# node and a number Output are made from the video the workflow makes); the slider then says what the clip asks for and changes nothing
# on the timeline; after a run the clip is as long as the video that came out and the clip after it slides right. Mock engine.
#   .\tools\uitest\workflow_length.ps1

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
      [pscustomobject]@{ Name = $_.name; At = [math]::Round((ConvertFrom-Rational $o.timing.record_in), 3); Len = [math]::Round((ConvertFrom-Rational $o.timing.duration), 3); Media = $o.media_ref }
    } | Sort-Object At)
  }
  function Show($l) { ($l | ForEach-Object { "$($_.Name)@$($_.At)+$($_.Len)" }) -join ', ' }

  # Two 5-second shots; the first gets its length from its workflow. The slider at 0.5 asks for 7.5 s of the mock model's 0.1 to 15.
  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
    'click @rail:Generate'
    'click @model:attome-mock'
    'wait 500'
    'click @model:attome-mock'
    'wait 500'
    'click @clip:Shot_1'
    'wait 400'
    'expect @combo:length_by'
    'click @combo:length_by'
    'click @option:length_from_video'        # a Get Duration node and an Output "length" on the clip's own workflow
    'wait 500'
    'slide @slider:gen_length 0.5'           # what it asks for: the timeline does not move
    'wait 500'
    "shot $work\length_asks.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $l = Layout $run; "before the run: $(Show $l)   asks for $($l[0].Media.asked_length) from $($l[0].Media.length_from)"
      if ($l[0].Media.length_from -ne 'length') { $failed = 'the clip does not take its length from the workflow' }
      elseif ([math]::Abs([double]$l[0].Media.asked_length - 7.5) -gt 0.01) { $failed = "the clip asks for $($l[0].Media.asked_length), not 7.5" }
      elseif ($l[0].Len -ne 5 -or $l[1].At -ne 5) { $failed = 'the timeline moved before anything was generated' }
    }
  } finally { }
  if (-not $failed) {
    $go = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 240 -Script @(
      'click @clip:Shot_1'
      'wait 400'
      'click @button:gen_run'
      'wait 300'
      'expect @button:gen_run'
      'wait 1800'
      "shot $work\length_after.jpg"
    )
    $failed = $go.Errors
    if (-not $failed) {
      $l = Layout $run; "after the run: $(Show $l)"
      $st = (Invoke-Attome $run --json gen status $proj | ConvertFrom-Json).result.clips
      if ([math]::Abs($l[0].Len - 7.5) -gt 0.05) { $failed = "the clip is $($l[0].Len) s, not as long as what the workflow made (7.5)" }
      elseif ([math]::Abs($l[1].At - $l[0].Len) -gt 0.05) { $failed = "the clip after it is at $($l[1].At), not right after the first ($($l[0].Len))" }
      elseif (($st | Where-Object { $_.name -eq 'Shot 1' }).state -ne 'clean') { $failed = 'the clip is out of date after its own run' }
      else {
        $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
        if (-not $valid.result.ok) { $failed = 'the project is not valid' }
      }
    }
  }
  Stop-Daemon $run
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a workflow that decides its clip's length sets it after the run, and the clip after it slides right (captures in $work)" -ForegroundColor Green
exit 0
