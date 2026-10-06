param([Parameter(Mandatory)][string]$Project)
# Run from the repository root:  powershell -NoProfile -File <this script> -Project <path>.attome
$ErrorActionPreference = 'Stop'
$bin = "build\win-msvc-release\bin"  # the repo root is the working directory
$p = $Project
function Call($tool, $o) {
  $f = "$env:TEMP\call2.json"
  [IO.File]::WriteAllText($f, ($o | ConvertTo-Json -Depth 30 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = (& $bin\attome.exe --json call $tool $f | ConvertFrom-Json)
  if (-not $r.ok) { throw "$tool failed: $($r.error.message) $($r.error.hint)" }
  $r.result
}
$info = (& $bin\attome.exe --json inspect $p --level tracks | ConvertFrom-Json).result.data
$seq = $info.sequences[0].id
$video = $info.sequences[0].tracks[0].id

# Scenes: name, start second, end second, narration. Together about 89 words.
$scenes = @(
 @{ s = 0;  e = 3;  label = $null;      say = "What if you were invisible for twenty-four hours? Let's find out." },
 @{ s = 3;  e = 7;  label = 'HOUR 1';   say = "Hour one: you vanish! Free popcorn, no ticket, no problem." },
 @{ s = 7;  e = 11; label = 'HOUR 3';   say = "Hour three: school pranks. The teacher's pencil is haunted." },
 @{ s = 11; e = 15; label = 'HOUR 6';   say = "Hour six: skip every line at the theme park!" },
 @{ s = 15; e = 19; label = 'HOUR 9';   say = "Hour nine: coffee time. Everyone watches it flow inside you. Gross." },
 @{ s = 19; e = 23; label = 'HOUR 12';  say = "Hour twelve: your clothes? Still very visible. Awkward!" },
 @{ s = 23; e = 27; label = 'HOUR 18';  say = "Hour eighteen: cars can't see you. Cross carefully!" },
 @{ s = 27; e = 31; label = 'HOUR 24';  say = "Hour twenty-four: nobody notices you. Not even at your party." },
 @{ s = 31; e = 36; label = 'THE TWIST'; say = "Then it wears off, on stage, in pyjamas. Follow for more What Ifs!" }
)
$key = @('invisible', 'vanish', 'free', 'popcorn', 'haunted', 'skip', 'gross', 'visible', 'awkward', 'cars', 'nobody', 'pyjamas', 'follow', 'twenty-four', 'party')
$totalWords = 0; $scenes | ForEach-Object { $totalWords += ($_.say -split '\s+').Count }
"script words: $totalWords"

function F($sec) { [int][math]::Round($sec * 30) }
function T($frames) { "$frames@30" }

$ops = New-Object System.Collections.ArrayList
# Four tracks above the pictures: shadows under the text. The first added sits nearest the pictures.
[void]$ops.Add(@{ op = 'add'; path = "$seq/tracks/`$new:capshadow"; value = @{ kind = 'video'; name = 'Caption shadow' }; anchor = @{ before = $video } })
[void]$ops.Add(@{ op = 'add'; path = "$seq/tracks/`$new:caps"; value = @{ kind = 'video'; name = 'Captions' }; anchor = @{ before = '$new:capshadow' } })
[void]$ops.Add(@{ op = 'add'; path = "$seq/tracks/`$new:titleshadow"; value = @{ kind = 'video'; name = 'Title shadow' }; anchor = @{ before = '$new:caps' } })
[void]$ops.Add(@{ op = 'add'; path = "$seq/tracks/`$new:titles"; value = @{ kind = 'video'; name = 'Titles' }; anchor = @{ before = '$new:titleshadow' } })

function AddText($track, $name, $text, $at, $frames, $size, $color, $x, $y) {
  [void]$ops.Add(@{ op = 'add'; path = "`$new:$track/clips/`$new:t$($script:n)"; value = @{
      name = $name; timing = @{ record_in = (T $at); duration = (T $frames); source_in = '0' }
      media_ref = @{ type = 'text' }
      content = @{ text = $text; size = $size; color = $color; bold = $true }
      transform = @{ position = @(@($x, $y)); opacity = 1.0 } } })
  $script:n++
}
$script:n = 0
$off = 0.0035   # the black copy sits a little down and right: a thick shadow
foreach ($sc in $scenes) {
  $words = @($sc.say -split '\s+')
  $a = $sc.s + 0.12; $b = $sc.e - 0.12
  $slot = ($b - $a) / $words.Count
  for ($i = 0; $i -lt $words.Count; ++$i) {
    $f0 = F ($a + $i * $slot); $f1 = F ($a + ($i + 1) * $slot)
    if ($i -eq $words.Count - 1) { $f1 = F $b }
    $frames = [math]::Max(1, $f1 - $f0)
    $w = $words[$i].ToUpper()
    $bare = ($words[$i].ToLower() -replace "[^a-z-]", '')
    $color = if ($key -contains $bare) { '#FFE600' } else { '#FFFFFF' }
    AddText 'capshadow' "shadow $w" $w $f0 $frames 0.07 '#000000' (0.5 + $off) (0.70 + $off)
    AddText 'caps' $w $w $f0 $frames 0.07 $color 0.5 0.70
  }
  if ($sc.label) {
    $lf0 = F ($sc.s + 0.1); $lf = F 1.5
    AddText 'titleshadow' "shadow $($sc.label)" $sc.label $lf0 $lf 0.055 '#000000' (0.5 + $off) (0.14 + $off)
    AddText 'titles' $sc.label $sc.label $lf0 $lf 0.055 '#FFE600' 0.5 0.14
  }
}
# The hook title over the first three seconds; the call to follow over the last two.
$hook = "WHAT IF YOU WERE`nINVISIBLE FOR`n24 HOURS?"
AddText 'titleshadow' 'shadow hook title' $hook 0 (F 3) 0.075 '#000000' (0.5 + $off) (0.27 + $off)
AddText 'titles' 'Hook title' $hook 0 (F 3) 0.075 '#FFFFFF' 0.5 0.27
$cta = "FOLLOW FOR MORE`nWHAT IFS!"
AddText 'titleshadow' 'shadow CTA' $cta (F 34) (F 2) 0.075 '#000000' (0.5 + $off) (0.40 + $off)
AddText 'titles' 'Call to follow' $cta (F 34) (F 2) 0.075 '#FFE600' 0.5 0.40

"ops: $($ops.Count)"
$r = Call project.patch @{ project = $p; patch = @{ ops = @($ops); label = 'Titles and captions' } }
"revision $($r.revision)"

