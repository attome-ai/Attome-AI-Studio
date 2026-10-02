# UI test: select a clip, add a dissolve into the next one from the Inspector, see it on the timeline, remove it.
# Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\dissolve.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP 'attome-uitest'
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Dissolve.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# Two 3-second clips that touch at 3 s, each with media to spare beyond the cut.
$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" sample "$work\a.mp4" --seconds 6 --height 540 | Out-Null
  & "$bin\attome.exe" sample "$work\b.mp4" --seconds 6 --height 540 | Out-Null
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $seq = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].id
  $a = ("$work\a.mp4").Replace('\', '\\')
  $b = ("$work\b.mp4").Replace('\', '\\')
  @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"a.mp4","timing":{"record_in":"0","duration":"3","source_in":"0"},"media_ref":{"type":"file","path":"$a","duration":"6","width":960,"height":540}}},
 {"op":"add","path":"`$new:v1/clips/`$new:b","value":{"name":"b.mp4","timing":{"record_in":"3","duration":"3","source_in":"2"},"media_ref":{"type":"file","path":"$b","duration":"6","width":960,"height":540}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
  & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Get-Transitions($ed) {
  $tracks = (Invoke-Attome $ed --json get $proj ((Invoke-Attome $ed --json inspect $proj --level tracks | ConvertFrom-Json).result.data.sequences[0].tracks[0].id) | ConvertFrom-Json).result.object
  if ($tracks.transitions) { @($tracks.transitions.PSObject.Properties | ForEach-Object { $_.Value }) } else { @() }
}

# Polls, because the edit lands a frame or two after the click.
function Wait-Transitions($ed, [int]$Count) {
  for ($i = 0; $i -lt 15; $i++) {
    $t = @(Get-Transitions $ed)
    if ($t.Count -eq $Count) { return ,$t }
    Start-Sleep -Milliseconds 200
  }
  ,$t
}

$ed = Start-Editor -Project $proj -SelectFirstClip
$failed = $null
try {
  Show-Window $ed
  Scroll-At $ed 1430 400 10 # the Transition card sits below Transform in the Inspector
  Save-Shot $ed (Join-Path $work 'dissolve_selected.png') | Out-Null
  Move-Mouse $ed 1350 309; Start-Sleep -Milliseconds 500 # hover first, so the button is hot when pressed
  Click-At $ed 1350 309 # Inspector: "Dissolve into next clip"
  $t = Wait-Transitions $ed 1
  Save-Shot $ed (Join-Path $work 'dissolve_added.png') | Out-Null
  "transitions: $($t.Count)  $($t | ConvertTo-Json -Compress)"
  if ($t.Count -ne 1 -or $t[0].type -ne 'attome.dissolve' -or $t[0].in_offset -ne '1/2' -or $t[0].out_offset -ne '1/2') {
    $failed = 'the Inspector did not add a 1-second dissolve centred on the cut'
  } else {
    Move-Mouse $ed 1350 311; Start-Sleep -Milliseconds 500
    Click-At $ed 1350 309 # the same place now holds "Remove dissolve"
    if ((Wait-Transitions $ed 0).Count -ne 0) { $failed = 'the Inspector did not remove the dissolve' }
  }
} finally { Stop-Editor $ed }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a dissolve was added and removed from the Inspector (captures in $work)" -ForegroundColor Green
