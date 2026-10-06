param([Parameter(Mandatory)][string]$Project)
# Run from the repository root:  powershell -NoProfile -File <this script> -Project <path>.attome
$ErrorActionPreference = 'Stop'
$bin = "build\win-msvc-release\bin"  # the repo root is the working directory
$p = $Project
function Call($tool, $o) {
  $f = "$env:TEMP\call.json"
  [IO.File]::WriteAllText($f, ($o | ConvertTo-Json -Depth 20 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = (& $bin\attome.exe --json call $tool $f | ConvertFrom-Json)
  if (-not $r.ok) { throw "$tool failed: $($r.error.message)" }
  $r.result
}
$model = 'minimax-h3.fl2va.turbo8-int8'
$scenes = @(
 @{ n = 'Hour 1';  s = 4; p = 'Inside a movie theater, {character}, now a faint see-through outline, grabs a floating bucket of popcorn from the snack counter while the cashier looks around, confused. Quick pop-in motion. {style}' },
 @{ n = 'Hour 3';  s = 4; p = 'A bright school classroom. {character}, see-through, sneaks behind the teacher and makes a pencil float up and wave around while the students gasp. Bouncy motion. {style}' },
 @{ n = 'Hour 6';  s = 4; p = 'A colourful theme park. {character}, a see-through outline, skips a huge queue and hops straight into a roller coaster seat while nobody notices. Light camera shake, fast zoom-in. {style}' },
 @{ n = 'Hour 9';  s = 4; p = 'A cosy cafe. {character}, see-through, drinks a cup of brown coffee and the coffee visibly flows down through the transparent body while a shocked waiter stares. {style}' },
 @{ n = 'Hour 12'; s = 4; p = 'A bedroom. {character} puts on a hoodie and jeans, and only the floating clothes are visible as they walk and dance around by themselves, the body see-through. Funny bounce. {style}' },
 @{ n = 'Hour 18'; s = 4; p = 'A busy road crossing. {character}, see-through, steps out and a car screeches to a stop, the driver honking and staring confused at an empty road. Quick zoom-in. {style}' },
 @{ n = 'Hour 24'; s = 4; p = 'A birthday party in a living room. {character}, see-through, waves sadly at friends but nobody notices; the friends look straight through the glowing outline next to the cake. Slow sad zoom. {style}' },
 @{ n = 'Twist';   s = 5; p = 'The invisibility wears off at the worst moment: {character} suddenly pops back into full colour on a school stage in front of a huge shocked audience, wearing funny polka dot pyjamas, confetti pops. Fast zoom-out. {style}' }
)
foreach ($sc in $scenes) {
  $r = Call gen.create_clip @{ project = $p; model = $model; seconds = $sc.s; name = $sc.n; prompt = $sc.p }
  "{0}: {1}" -f $sc.n, $r.clip
}

