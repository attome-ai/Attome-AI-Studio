# UI test: drag the Inspector's "Fade in" slider; the clip gets opacity keys that rise from 0; split keeps them valid.
# Virtual input only (uitest.psm1). Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\fade.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Fade.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 6 --height 540"
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

function Get-OpacityKeys($clip) {
  $k = $clip.transform.keyframes.opacity
  if (-not $k) { return ,@() }
  ,@($k.PSObject.Properties | ForEach-Object { $_.Value } | Sort-Object { ConvertFrom-Rational $_.t })
}
function Get-Clips($run) { ,@((Get-Tracks $run)[0].clip_list | ForEach-Object { Get-Object $run $_.id }) }

# The 4-second clip is selected; each fade slider spans 0..4 s, so a quarter is 1 s.
$run = Invoke-EditorScript -Project $proj -SelectFirstClip -Script @(
  'click @button:add_card_fade'
  'slide @slider:fadein 0.25'
  'slide @slider:fadeout 0.25'
  "shot $work\fade_after.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $keys = Get-OpacityKeys (Get-Clips $run)[0]
    "after the fades: $($keys | ConvertTo-Json -Compress)"
    $rise = if ($keys.Count -eq 4) { ConvertFrom-Rational $keys[1].t } else { 0 }
    if ($keys.Count -ne 4 -or $keys[0].t -ne '0' -or $keys[0].v -ne 0 -or $keys[3].t -ne '4' -or $keys[3].v -ne 0 -or
        $rise -lt 0.8 -or $rise -gt 1.2) {
      $failed = 'the Fade card did not make a 1 s fade in from 0 and a fade out to 0 at the end'
    }
  }
  if (-not $failed) { # S splits at the playhead (2 s): each half keeps the fade at its outer end
    $split = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('key S', "shot $work\fade_split.jpg")
    $failed = $split.Errors
    if (-not $failed) {
      $clips = Get-Clips $run
      $l = Get-OpacityKeys $clips[0]; $r = Get-OpacityKeys $clips[1]
      "after split: left $($l | ConvertTo-Json -Compress)  right $($r | ConvertTo-Json -Compress)"
      if ($clips.Count -ne 2) { $failed = 'split did not make two clips' }
      elseif ($l.Count -ne 2 -or $l[0].v -ne 0 -or $r.Count -ne 2 -or $r[1].v -ne 0 -or $r[1].t -ne '2') {
        $failed = 'after a split the left half should keep the fade in and the right half the fade out'
      }
    }
  }
  if (-not $failed) { # the number is typed into: an exact length, longer than a drag would hit
    $typed = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('expect @number:fadein', 'click @number:fadein', 'wait 300', 'type 1.5', 'key Enter', 'wait 700', "shot $work\fade_typed.jpg")
    $failed = $typed.Errors
    if (-not $failed) {
      $l = Get-OpacityKeys (Get-Clips $run)[0]
      "after typing 1.5: $($l | ConvertTo-Json -Compress)"
      if ($l.Count -ne 2 -or [math]::Abs((ConvertFrom-Rational $l[1].t) - 1.5) -gt 0.02) { $failed = 'typing 1.5 into the fade-in number did not make a 1.5 s fade' }
    }
  }
  if (-not $failed) { # a double click on the slider sets it back: no fade in
    $reset = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('dblclick @slider:fadein', 'wait 800')
    $failed = $reset.Errors
    if (-not $failed -and (Get-OpacityKeys (Get-Clips $run)[0]).Count -ne 0) { $failed = 'a double click on the fade-in slider did not take the fade away' }
  }
  if (-not $failed) { # Remove fade takes the fades off and the card away; the clip offers Fade again
    $gone = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @(
      'click @button:add_card_fade', 'wait 300', 'click @number:fadeout', 'wait 300', 'type 1', 'key Enter', 'wait 700'
      'expect @button:remove_fade', 'click @button:remove_fade', 'wait 800'
      'absent @slider:fadein', 'expect @button:add_card_fade')
    $failed = $gone.Errors
    if (-not $failed -and (Get-OpacityKeys (Get-Clips $run)[0]).Count -ne 0) { $failed = 'Remove fade left opacity keys on the clip' }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: fades from the Inspector, kept through a split, typed as a number, set back by a double click and removed (captures in $work)" -ForegroundColor Green
exit 0
