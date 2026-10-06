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
# One keyframe of a property of a clip: time in frames from its start, a value, and an ease (or none: linear).
function Key($clip, $prop, $frame, $value, $ease) {
  $v = @{ t = (T $frame); v = $value }
  if ($ease) { $v.interp = 'easing'; $v.ease = $ease }
  $script:k++
  [void]$ops.Add(@{ op = 'add'; path = "$clip/transform/keyframes/$prop/`$new:k$($script:k)"; value = $v })
}

# ---- the pictures: a punch-in at every cut, then a slow push; a shake on two scenes, a sway on one ----------------------------
$vi = 0
foreach ($c in (TrackClips 'V1')) {
  $dur = Fr $c.C.timing.duration
  $s = [double]$c.C.transform.scale[0]
  Key $c.Id 'scale' 0 @(($s * 1.20), ($s * 1.20)) 'ease_out_expo'
  Key $c.Id 'scale' 10 @(($s * 1.02), ($s * 1.02)) $null
  Key $c.Id 'scale' $dur @(($s * 1.12), ($s * 1.12)) $null
  if ($vi -in 3, 6) { # the roller coaster and the car: a shake
    $off = @(0.007, -0.006, 0.006, -0.005, 0.004, -0.003, 0.002)
    Key $c.Id 'position' 0 @(0.5, 0.5) $null
    for ($i = 0; $i -lt $off.Count; ++$i) { Key $c.Id 'position' (2 + 2 * $i) @((0.5 + $off[$i]), (0.5 - $off[$i] * 0.7)) $null }
    Key $c.Id 'position' 18 @(0.5, 0.5) $null
  }
  if ($vi -eq 5) { # the dancing clothes: a sway
    $sign = 1
    for ($f = 0; $f -le $dur; $f += 12) { Key $c.Id 'rotation' $f (2.0 * $sign) 'ease_in_out_quad'; $sign = -$sign }
  }
  ++$vi
}

# ---- the text: every word pops in; the labels pop and slide; the hook and the call to follow have their own motion ----------------
function PopWord($clip, $dur, $big) {
  if ($dur -lt 3) { return }
  $settle = [math]::Min(5, $dur - 1)
  Key $clip 'scale' 0 @(0.55, 0.55) 'ease_out_back'
  if ($big -and $dur -ge 10) {
    Key $clip 'scale' 4 @(1.28, 1.28) 'ease_out_quad'
    Key $clip 'scale' 9 @(1.0, 1.0) $null
  } else {
    Key $clip 'scale' $settle @(1.0, 1.0) $null
  }
}
$keyWords = @('INVISIBLE', 'VANISH!', 'FREE', 'POPCORN,', 'HAUNTED.', 'SKIP', 'GROSS.', 'VISIBLE.', 'AWKWARD!', 'CARS', 'NOBODY', 'PYJAMAS.', 'FOLLOW', 'TWENTY-FOUR:', 'PARTY.')
foreach ($track in 'Captions', 'Caption shadow') {
  foreach ($c in (TrackClips $track)) {
    $word = ($c.C.name -replace '^shadow ', '')
    PopWord $c.Id (Fr $c.C.timing.duration) ($keyWords -contains $word)
  }
}
foreach ($track in 'Titles', 'Title shadow') {
  foreach ($c in (TrackClips $track)) {
    $dur = Fr $c.C.timing.duration
    $x = [double]$c.C.transform.position[0]; $y = [double]$c.C.transform.position[1]
    $name = $c.C.name
    if ($name -match 'Hook title|hook title') { # pops in with a bounce, then breathes
      Key $c.Id 'scale' 0 @(0.35, 0.35) 'ease_out_back'
      Key $c.Id 'scale' 12 @(1.0, 1.0) 'ease_in_out_quad'
      Key $c.Id 'scale' 36 @(1.07, 1.07) 'ease_in_out_quad'
      Key $c.Id 'scale' 60 @(1.0, 1.0) 'ease_in_out_quad'
      Key $c.Id 'scale' ($dur - 1) @(1.05, 1.05) $null
    } elseif ($name -match 'CTA|Call to follow') { # a pulse, over and over
      $up = $true
      for ($f = 0; $f -le $dur; $f += 9) {
        $v = if ($up) { 1.0 } else { 1.16 }
        Key $c.Id 'scale' $f @($v, $v) 'ease_in_out_quad'
        $up = -not $up
      }
    } else { # an hour label: drops in from above with a bounce, and fades at its end
      Key $c.Id 'scale' 0 @(0.4, 0.4) 'ease_out_back'
      Key $c.Id 'scale' 8 @(1.0, 1.0) $null
      Key $c.Id 'position' 0 @($x, ($y - 0.05)) 'ease_out_back'
      Key $c.Id 'position' 9 @($x, $y) $null
      Key $c.Id 'opacity' ($dur - 7) 1.0 $null
      Key $c.Id 'opacity' $dur 0.0 $null
    }
  }
}

# ---- two adjustment layers between the pictures and the text: a flash on every cut, and one look over the whole film ------------
$grade = @{ op = 'add'; path = "$seq/tracks/`$new:grade"; value = @{ kind = 'video'; name = 'Grade' }; anchor = @{ before = $byName['Caption shadow'].id } }
$flash = @{ op = 'add'; path = "$seq/tracks/`$new:flash"; value = @{ kind = 'video'; name = 'Flash' }; anchor = @{ before = $byName['Caption shadow'].id } }
[void]$ops.Add($grade); [void]$ops.Add($flash)
[void]$ops.Add(@{ op = 'add'; path = "`$new:grade/clips/`$new:look"; value = @{
    name = 'Bright look'; timing = @{ record_in = (T 0); duration = (T 1080); source_in = '0' }; media_ref = @{ type = 'adjustment' }
    effects = @{
      '$new:fx1' = @{ effect = 'attome.color_grade@1.0.0'; enabled = $true; params = @{ brightness = 0.0; contrast = 0.12; saturation = 1.3 } }
      '$new:fx2' = @{ effect = 'attome.vignette@1.0.0'; enabled = $true; params = @{ strength = 0.3; radius = 0.6; softness = 0.5 } } }
    transform = @{ opacity = 1.0 } } })
$cutFrames = 90, 210, 330, 450, 570, 690, 810, 930
$n = 0
foreach ($f0 in $cutFrames) {
  ++$n
  [void]$ops.Add(@{ op = 'add'; path = "`$new:flash/clips/`$new:flash$n"; value = @{
      name = "Flash $n"; timing = @{ record_in = (T $f0); duration = (T 7); source_in = '0' }; media_ref = @{ type = 'adjustment' }
      effects = @{ "`$new:fxf$n" = @{ effect = 'attome.color_grade@1.0.0'; enabled = $true
          params = @{ brightness = 0.0; contrast = 0.0; saturation = 1.0 }
          keyframes = @{ brightness = @{
              "`$new:b0$n" = @{ t = (T 0); v = 0.65; interp = 'easing'; ease = 'ease_out_quad' }
              "`$new:b1$n" = @{ t = (T 6); v = 0.0 } } } } }
      transform = @{ opacity = 1.0 } } })
}

"ops: $($ops.Count)"
$f = "$env:TEMP\anim.json"
[IO.File]::WriteAllText($f, (@{ project = $p; patch = @{ ops = @($ops); label = 'Animation' } } | ConvertTo-Json -Depth 12 -Compress), (New-Object Text.UTF8Encoding($false)))
$r = & $bin\attome.exe --json call project.patch $f | ConvertFrom-Json
if (-not $r.ok) { "FAILED: $($r.error.message)"; $r.error.hint; $r.error.path } else { "revision $($r.result.revision)" }

