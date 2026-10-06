param([Parameter(Mandatory)][string]$Project)
# Run from the repository root:  powershell -NoProfile -File <this script> -Project <path>.attome
$ErrorActionPreference = 'Stop'
$bin = "build\win-msvc-release\bin"  # the repo root is the working directory
$p = $Project
function Fr($rational) { $x = [string]$rational; if ($x -match '/') { $a = $x -split '/'; [int][math]::Round(30 * [double]$a[0] / [double]$a[1]) } else { [int][math]::Round(30 * [double]$x) } }
$tr = (& $bin\attome.exe --json inspect $p --level tracks | ConvertFrom-Json).result.data.sequences[0].tracks | Where-Object { $_.name -eq 'V1' }
# the shots a scene is cut into: wide, close on the upper middle (faces), medium to a side, tight; each pushes in a little while it lasts
$plan = @(
  @{ m0 = 1.00; m1 = 1.08; fx = 0.50; fy = 0.50 },
  @{ m0 = 1.50; m1 = 1.64; fx = 0.50; fy = 0.38 },
  @{ m0 = 1.30; m1 = 1.40; fx = 0.62; fy = 0.55 },
  @{ m0 = 1.78; m1 = 1.92; fx = 0.44; fy = 0.42 })
$ops = New-Object System.Collections.ArrayList
$script:n = 0
function Key($clip, $prop, $frame, $value, $ease) {
  $v = @{ t = "$frame@30"; v = $value }
  if ($ease) { $v.interp = 'easing'; $v.ease = $ease }
  $script:n++
  [void]$ops.Add(@{ op = 'add'; path = "$clip/transform/keyframes/$prop/`$new:s$($script:n)"; value = $v })
}
function Pos($m, $fx, $fy) {
  if ($m -le 1.08) { return @(0.5, 0.5) }
  @((0.5 + $m * (0.5 - $fx) * 0.9), (0.5 + $m * (0.5 - $fy) * 0.9))
}
$ci = 0; $cuts = 0
foreach ($ref in $tr.clip_list) {
  $o = (& $bin\attome.exe --json get $p $ref.id | ConvertFrom-Json).result.object
  $dur = Fr $o.timing.duration
  $base = [double]$o.transform.scale[0]
  $shots = if ($dur -le 90) { 3 } else { [math]::Max(2, [int][math]::Round($dur / 40.0)) }
  $len = [int][math]::Floor($dur / $shots)
  $flip = if ($ci % 2 -eq 0) { 1 } else { -1 }
  foreach ($prop in 'scale', 'position') { if ($o.transform.keyframes.PSObject.Properties.Name -contains $prop) { foreach ($kid in $o.transform.keyframes.$prop.PSObject.Properties.Name) { [void]$ops.Add(@{ op = 'remove'; path = $kid }) } } }
  for ($k = 0; $k -lt $shots; $k++) {
    $pl = $plan[$k % $plan.Count]
    $fs = $k * $len
    $fe = if ($k -eq $shots - 1) { $dur } else { ($k + 1) * $len - 1 }
    $fx = 0.5 + ($pl.fx - 0.5) * $flip
    $punch = if ($k -gt 0) { 1.07 } else { 1.0 }   # a cut lands with a small punch that settles in 6 frames
    $s0 = $base * $pl.m0; $s1 = $base * $pl.m1
    Key $ref.id 'scale' $fs @(($s0 * $punch), ($s0 * $punch)) 'ease_out_quad'
    Key $ref.id 'scale' ($fs + 6) @($s0, $s0) $null
    Key $ref.id 'scale' $fe @($s1, $s1) $null
    Key $ref.id 'position' $fs (Pos $pl.m0 $fx $pl.fy) $null
    Key $ref.id 'position' $fe (Pos $pl.m1 $fx $pl.fy) $null
    if ($k -gt 0) { $cuts++ }
  }
  $ci++
}
"ops $($ops.Count), $cuts new cuts inside the scenes"
$f = "$env:TEMP\shots.json"
[IO.File]::WriteAllText($f, (@{ project = $p; patch = @{ ops = @($ops); label = 'A new shot every 1-1.5 s' } } | ConvertTo-Json -Depth 12 -Compress), (New-Object Text.UTF8Encoding($false)))
$r = & $bin\attome.exe --json call project.patch $f | ConvertFrom-Json
if (-not $r.ok) { "FAILED: $($r.error.message) $($r.error.data.hint) $($r.error.data.path)"; exit 1 } else { "revision $($r.result.revision)" }



