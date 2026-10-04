# UI test: keyframes on an effect parameter. A vignette layer from the Effects panel gets two keys on its strength from
# the diamond and the slider, a key is removed and added again, Next key steps to a key, and a split moves the keys to
# the right half. Virtual input only (uitest.psm1). Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\effect_keys.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'EffectKeys.attome'
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
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"a.mp4","timing":{"record_in":"0","duration":"6","source_in":"0"},"media_ref":{"type":"file","path":"$a","duration":"6","width":960,"height":540}}}
]}
"@ | Set-Content "$work\pk.json" -Encoding utf8
  & "$bin\attome.exe" patch $proj "$work\pk.json" | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

# The strength keys of the first effect of a clip, as @{ t; v } sorted by time (t as a number of seconds).
function Get-Keys($run, $clipId) {
  $fx = @((Get-Object $run $clipId).effects.PSObject.Properties | ForEach-Object { $_.Value })[0]
  if (-not $fx.keyframes -or -not $fx.keyframes.strength) { return ,@() }
  ,@($fx.keyframes.strength.PSObject.Properties | ForEach-Object { [pscustomobject]@{ t = (ConvertFrom-Rational $_.Value.t); v = [double]$_.Value.v } } | Sort-Object t)
}

$failed = $null
$run = Invoke-EditorScript -Project $proj -Script @(
  'click @rail:Effects'
  'click @effect:vignette'                  # a vignette layer from 0 to 3 s, selected, the playhead in its middle (1.5 s)
  'click @key:vignette_strength'            # animate: a key at 1.5 s with the plain value
  'slide @slider:vignette_strength 1'       # the slider now edits that key: strength 1 at 1.5 s
  'key Home'                                # the playhead to 0
  'click @key:vignette_strength'            # a second key at 0, holding what the curve gives there (1)
  'slide @slider:vignette_strength 0'       # and set to 0: the vignette closes in over the first 1.5 s
  'wait 800'                                # the Monitor renders on its own thread: let it catch up before the capture
  "shot $work\effect_keys_start.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $id = (@(Get-Tracks $run) | Where-Object { $_.name -eq 'Effects' }).clip_list[0].id
    $keys = Get-Keys $run $id
    $plain = @((Get-Object $run $id).effects.PSObject.Properties | ForEach-Object { $_.Value })[0].params.strength
    "keys: $(($keys | ForEach-Object { '{0}s={1}' -f $_.t, $_.v }) -join '  ')   plain strength: $plain"
    if ($keys.Count -ne 2) { $failed = "expected 2 strength keys, found $($keys.Count)" }
    elseif ([math]::Abs($keys[0].t) -gt 0.001 -or [math]::Abs($keys[0].v) -gt 0.01) { $failed = 'the first key is not strength 0 at 0 s' }
    elseif ([math]::Abs($keys[1].t - 1.5) -gt 0.001 -or [math]::Abs($keys[1].v - 1.0) -gt 0.01) { $failed = 'the second key is not strength 1 at 1.5 s' }
    elseif ([math]::Abs($plain - 0.5) -gt 0.001) { $failed = 'the plain strength changed: the slider should have edited the keys' }
  }
  if (-not $failed) { # remove the key at 0 with the diamond, then add it back; Next key to 1.5 s and split there
    $again = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'click @rail:Effects'
      'click @clip:Vignette'
      'key Home'
      'click @key:vignette_strength'          # the diamond on the key at 0 removes it
    )
    $failed = $again.Errors
    if (-not $failed) {
      $id = (@(Get-Tracks $run) | Where-Object { $_.name -eq 'Effects' }).clip_list[0].id
      $keys = Get-Keys $run $id
      "after removing the key at 0: $(($keys | ForEach-Object { '{0}s={1}' -f $_.t, $_.v }) -join '  ')"
      if ($keys.Count -ne 1) { $failed = "the diamond did not remove the key at 0 (found $($keys.Count) keys)" }
    }
  }
  if (-not $failed) { # put it back with the diamond, step with Next key, split at the key
    $third = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'click @rail:Effects'
      'click @clip:Vignette'
      'key Home'
      'click @key:vignette_strength'          # a key at 0 again (holding the value the curve gives: 1)
      'click @button:next_key_vignette'       # the playhead jumps to the key at 1.5 s
      'key S'                                 # split the layer there
      "shot $work\effect_keys_split.jpg"
    )
    $failed = $third.Errors
    if (-not $failed) {
      $clips = @((@(Get-Tracks $run) | Where-Object { $_.name -eq 'Effects' }).clip_list)
      if ($clips.Count -ne 2) { $failed = "the split made $($clips.Count) layers, expected 2" }
      else {
        $left = Get-Keys $run $clips[0].id
        $right = Get-Keys $run $clips[1].id
        "left keys:  $(($left | ForEach-Object { '{0}s={1}' -f $_.t, $_.v }) -join '  ')"
        "right keys: $(($right | ForEach-Object { '{0}s={1}' -f $_.t, $_.v }) -join '  ')"
        if ($left.Count -ne 2 -or $right.Count -ne 2) { $failed = 'each half should carry both keys' }
        elseif ([math]::Abs($right[0].t + 1.5) -gt 0.001 -or [math]::Abs($right[1].t) -gt 0.001) { $failed = 'the right half keys are not local to it (-1.5 s and 0 s)' }
      }
    }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: keys on an effect parameter from the diamond and slider, removed, stepped to and split (captures in $work)" -ForegroundColor Green
exit 0
