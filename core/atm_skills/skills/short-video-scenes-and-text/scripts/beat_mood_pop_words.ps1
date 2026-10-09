param([Parameter(Mandatory)][string]$Project)
# Run from the repository root:  powershell -NoProfile -File <this script> -Project <path>.attome
$ErrorActionPreference = 'Stop'
$bin = "build\win-msvc-release\bin"  # the repo root is the working directory
$p = $Project
$info = (& $bin\attome.exe --json inspect $p | ConvertFrom-Json).result.data
$seq = $info.sequences[0].id
$tracks = (& $bin\attome.exe --json inspect $p --level tracks | ConvertFrom-Json).result.data.sequences[0].tracks
$byName = @{}; foreach ($t in $tracks) { $byName[$t.name] = $t }
function TrackClips($name) {
  $o = (& $bin\attome.exe --json get $p $byName[$name].id | ConvertFrom-Json).result.object
  foreach ($c in $o.clip_order) { [pscustomobject]@{ Id = $c; C = $o.clips.$c } }
}
function Fr($rational) { $x = [string]$rational; if ($x -match '/') { $a = $x -split '/'; [int][math]::Round(30 * [double]$a[0] / [double]$a[1]) } else { [int][math]::Round(30 * [double]$x) } }
function T($frames) { "$frames@30" }
$ops = New-Object System.Collections.ArrayList
$script:k = 0
function Key($clip, $prop, $frame, $value, $ease) {
  $v = @{ t = (T $frame); v = $value }
  if ($ease) { $v.interp = 'easing'; $v.ease = $ease }
  $script:k++
  [void]$ops.Add(@{ op = 'add'; path = "$clip/transform/keyframes/$prop/`$new:k$($script:k)"; value = $v })
}

# 1. A beat under the pictures: a small push on every half second, riding on the slow zoom they already have.
$v1 = @(TrackClips 'V1')
foreach ($c in $v1) {
  $dur = Fr $c.C.timing.duration
  $s = [double]$c.C.transform.scale[0]
  for ($t = 20; $t -lt $dur - 9; $t += 15) {
    $ramp = 1.02 + 0.10 * ($t - 10) / ($dur - 10)
    $rampLow = 1.02 + 0.10 * ($t + 7 - 10) / ($dur - 10)
    Key $c.Id 'scale' $t @(($s * $ramp * 1.035), ($s * $ramp * 1.035)) 'ease_out_quad'
    Key $c.Id 'scale' ($t + 7) @(($s * $rampLow), ($s * $rampLow)) 'ease_in_out_quad'
  }
}

# 2. A whip of blur on every cut, on top of the white flash that is already there.
foreach ($c in (TrackClips 'Flash')) {
  [void]$ops.Add(@{ op = 'add'; path = "$($c.Id)/effects/`$new:blur$($c.Id)"; value = @{
      effect = 'attome.gaussian_blur@1.0.0'; enabled = $true; params = @{ radius = 0.0 }
      keyframes = @{ radius = @{
          "`$new:r0$($c.Id)" = @{ t = (T 0); v = 0.05; interp = 'easing'; ease = 'ease_out_quad' }
          "`$new:r1$($c.Id)" = @{ t = (T 6); v = 0.0 } } } } })
}

# 3. A mood for some scenes, on a track of its own between the pictures and the text.
[void]$ops.Add(@{ op = 'add'; path = "$seq/tracks/`$new:mood"; value = @{ kind = 'video'; name = 'Mood' }; anchor = @{ before = $byName['Caption shadow'].id } })
function Mood($n, $from, $to, $effects) {
  [void]$ops.Add(@{ op = 'add'; path = "`$new:mood/clips/`$new:mood$n"; value = @{
      name = "Mood $n"; timing = @{ record_in = (T (30 * $from)); duration = (T (30 * ($to - $from))); source_in = '0' }
      media_ref = @{ type = 'adjustment' }; effects = $effects; transform = @{ opacity = 1.0 } } })
}
Mood 1 23 27 @{ '$new:m1a' = @{ effect = 'attome.color_grade@1.0.0'; enabled = $true; params = @{ brightness = -0.04; contrast = 0.2; saturation = 1.1 } }
                '$new:m1b' = @{ effect = 'attome.vignette@1.0.0'; enabled = $true; params = @{ strength = 0.6; radius = 0.5; softness = 0.4 } } }   # the car: tension
Mood 2 27 31 @{ '$new:m2a' = @{ effect = 'attome.color_grade@1.0.0'; enabled = $true; params = @{ brightness = -0.03; contrast = 0.12; saturation = 0.5 } }
                '$new:m2b' = @{ effect = 'attome.film_grain@1.0.0'; enabled = $true; params = @{ strength = 0.3; size = 2.0 } } }                   # the lonely party
Mood 3 31 36 @{ '$new:m3a' = @{ effect = 'attome.color_grade@1.0.0'; enabled = $true; params = @{ brightness = 0.05; contrast = 0.1; saturation = 1.45 } } }   # the twist: everything pops

# 4. Sound words that burst out at the key moments (above the picture's middle, clear of the labels and the captions).
[void]$ops.Add(@{ op = 'add'; path = "$seq/tracks/`$new:popshadow"; value = @{ kind = 'video'; name = 'Pop shadow' } })
[void]$ops.Add(@{ op = 'add'; path = "$seq/tracks/`$new:popwords"; value = @{ kind = 'video'; name = 'Pop words' } })
$bursts = @(
  @{ at = 1.9;  say = 'POOF!';      color = '#00E5FF'; y = 0.50 },
  @{ at = 4.3;  say = 'SNEAKY!';    color = '#FF3DCB'; y = 0.32 },
  @{ at = 8.3;  say = 'SPOOKY!';    color = '#B26BFF'; y = 0.32 },
  @{ at = 12.2; say = 'WHEEEE!';    color = '#FFB300'; y = 0.32 },
  @{ at = 16.3; say = 'GLUG GLUG';  color = '#7CFF3D'; y = 0.32 },
  @{ at = 20.2; say = 'BOING!';     color = '#00E5FF'; y = 0.32 },
  @{ at = 24.0; say = 'BEEP BEEP!'; color = '#FF3D3D'; y = 0.32 },
  @{ at = 28.2; say = 'HELLO...?';  color = '#9FB3C8'; y = 0.32 },
  @{ at = 31.7; say = 'SURPRISE!';  color = '#FFE600'; y = 0.32 })
$bi = 0
foreach ($b in $bursts) {
  ++$bi
  $f0 = [int][math]::Round(30 * $b.at); $dur = 26
  foreach ($layer in 'shadow', 'text') {
    $off = if ($layer -eq 'shadow') { 0.005 } else { 0.0 }
    $track = if ($layer -eq 'shadow') { 'popshadow' } else { 'popwords' }
    $id = "`$new:b$layer$bi"
    [void]$ops.Add(@{ op = 'add'; path = "`$new:$track/clips/$id"; value = @{
        name = "$layer $($b.say)"; timing = @{ record_in = (T $f0); duration = (T $dur); source_in = '0' }; media_ref = @{ type = 'text' }
        content = @{ text = $b.say; size = 0.16; color = $(if ($layer -eq 'shadow') { '#010101' } else { $b.color }); bold = $true }
        transform = @{ position = @(@((0.5 + $off), ($b.y + $off))); opacity = 1.0 } } })
    # a pop with a bounce, a wobble, and out
    $base = "$id/transform/keyframes"
    foreach ($kv in @(
        @('scale', 0, @(0.3, 0.3), 'ease_out_back'), @('scale', 7, @(1.3, 1.3), 'ease_in_out_quad'), @('scale', 12, @(1.0, 1.0), $null), @('scale', 25, @(1.08, 1.08), $null),
        @('rotation', 0, -10.0, 'ease_out_back'), @('rotation', 6, 6.0, 'ease_in_out_quad'), @('rotation', 12, -3.0, 'ease_in_out_quad'), @('rotation', 25, 2.0, $null),
        @('opacity', 20, 1.0, $null), @('opacity', 25, 0.0, $null))) {
      $script:k++
      $v = @{ t = (T $kv[1]); v = $kv[2] }
      if ($kv[3]) { $v.interp = 'easing'; $v.ease = $kv[3] }
      [void]$ops.Add(@{ op = 'add'; path = "$base/$($kv[0])/`$new:bk$($script:k)"; value = $v })
    }
  }
}
# The placeholders of the tracks added above were named for the clips' paths: the Pop shadow track sits under Pop words.
"ops: $($ops.Count)"
$f = "$env:TEMP\anim2.json"
[IO.File]::WriteAllText($f, (@{ project = $p; patch = @{ ops = @($ops); label = 'Beat, whip, mood and sound words' } } | ConvertTo-Json -Depth 14 -Compress), (New-Object Text.UTF8Encoding($false)))
$r = & $bin\attome.exe --json call project.patch $f | ConvertFrom-Json
if (-not $r.ok) { "FAILED: $($r.error.message)"; $r.error.hint; $r.error.path; exit 1 } else { "revision $($r.result.revision)" }

