# UI test: drag the Inspector's "Fade in" slider; the clip gets opacity keys that rise from 0; split keeps them valid.
# Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\fade.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP 'attome-uitest'
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Fade.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  & "$bin\attome.exe" sample "$work\a.mp4" --seconds 6 --height 540 | Out-Null
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $seq = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].id
  $a = ("$work\a.mp4").Replace('\', '\\')
  @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"a.mp4","timing":{"record_in":"0","duration":"4","source_in":"0"},"media_ref":{"type":"file","path":"$a","duration":"6","width":960,"height":540},"transform":{"opacity":1}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
  & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Get-Clips($ed) {
  $track = (Invoke-Attome $ed --json inspect $proj --level tracks | ConvertFrom-Json).result.data.sequences[0].tracks[0]
  @($track.clip_list | ForEach-Object { (Invoke-Attome $ed --json get $proj $_.id | ConvertFrom-Json).result.object })
}
function Get-OpacityKeys($clip) {
  $k = $clip.transform.keyframes.opacity
  if (-not $k) { return ,@() }
  $keys = @($k.PSObject.Properties | ForEach-Object { $_.Value })
  # sorted by time; times are canonical rationals such as "1" or "7/2"
  ,@($keys | Sort-Object { $p = $_.t -split '/'; [double]$p[0] / $(if ($p.Count -gt 1) { [double]$p[1] } else { 1.0 }) })
}
function Wait-Keys($ed, [int]$Count) {
  for ($i = 0; $i -lt 15; $i++) {
    $keys = Get-OpacityKeys (Get-Clips $ed)[0]
    if ($keys.Count -eq $Count) { return ,$keys }
    Start-Sleep -Milliseconds 200
  }
  ,$keys
}

$ed = Start-Editor -Project $proj -SelectFirstClip
$failed = $null
try {
  Show-Window $ed
  Scroll-At $ed 1430 400 10 # the Fade card sits below Transform in the Inspector
  Save-Shot $ed (Join-Path $work 'fade_before.png') | Out-Null
  Move-Drag $ed 1378 185 1412 185 # Fade in: about 1 s
  $keys = Wait-Keys $ed 2
  "after fade in: $($keys | ConvertTo-Json -Compress)"
  if ($keys.Count -ne 2 -or $keys[0].t -ne '0' -or $keys[0].v -ne 0 -or $keys[1].v -ne 1) { $failed = 'Fade in did not make keys 0 -> 1' }
  else {
    Move-Drag $ed 1378 212 1412 212 # Fade out: about 1 s
    $keys = Wait-Keys $ed 4
    "after fade out: $($keys | ConvertTo-Json -Compress)"
    if ($keys.Count -ne 4 -or $keys[3].t -ne '4' -or $keys[3].v -ne 0) { $failed = 'Fade out did not end at 0 at the clip end' }
  }
  Save-Shot $ed (Join-Path $work 'fade_after.png') | Out-Null
  if (-not $failed) {
    Send-Key $ed 0x53 # S: split at the playhead (2 s)
    Start-Sleep -Seconds 1
    $clips = Get-Clips $ed
    $l = Get-OpacityKeys $clips[0]; $r = Get-OpacityKeys $clips[1]
    "after split: left $($l | ConvertTo-Json -Compress)  right $($r | ConvertTo-Json -Compress)"
    Save-Shot $ed (Join-Path $work 'fade_split.png') | Out-Null
    if ($clips.Count -ne 2) { $failed = 'split did not make two clips' }
    elseif ($l.Count -ne 2 -or $l[0].v -ne 0 -or $r.Count -ne 2 -or $r[1].v -ne 0 -or $r[1].t -ne '2') {
      $failed = 'after a split the left half should keep the fade in and the right half the fade out'
    }
  }
} finally { Stop-Editor $ed }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: fades from the Inspector, kept through a split (captures in $work)" -ForegroundColor Green
