param([Parameter(Mandatory)][string]$Project)
# Run from the repository root:  powershell -NoProfile -File <this script> -Project <path>.attome
$ErrorActionPreference = 'Stop'
$bin = "build\win-msvc-release\bin"  # the repo root is the working directory
$p = $Project
function Call($tool, $o) {
  $f = "$env:TEMP\vo.json"
  [IO.File]::WriteAllText($f, ($o | ConvertTo-Json -Depth 20 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & $bin\attome.exe --json call $tool $f | ConvertFrom-Json
  if (-not $r.ok) { throw "$tool failed: $($r.error.message) $($r.error.hint)" }
  $r.result
}
# The earlier single test clip goes; the nine lines of the script come in at the starts of their scenes.
$tracks = (& $bin\attome.exe --json inspect $p --level tracks | ConvertFrom-Json).result.data.sequences[0].tracks
foreach ($t in $tracks) { foreach ($c in @($t.clip_list)) { if ($c.name -like 'Voice*') { Call project.patch @{ project = $p; patch = @{ ops = @(@{ op = 'remove'; path = $c.id }); label = 'remove test voice' } } | Out-Null } } }
$lines = @(
  @{ at = 0.15; say = "What if you were invisible for twenty-four hours? Let's find out." },
  @{ at = 3.1;  say = "Hour one: you vanish! Free popcorn, no ticket, no problem." },
  @{ at = 7.1;  say = "Hour three: school pranks. The teacher's pencil is haunted." },
  @{ at = 11.1; say = "Hour six: skip every line at the theme park!" },
  @{ at = 15.1; say = "Hour nine: coffee time. Everyone watches it flow inside you. Gross." },
  @{ at = 19.1; say = "Hour twelve: your clothes? Still very visible. Awkward!" },
  @{ at = 23.1; say = "Hour eighteen: cars can't see you. Cross carefully!" },
  @{ at = 27.1; say = "Hour twenty-four: nobody notices you. Not even at your party." },
  @{ at = 31.1; say = "Then it wears off, on stage, in pyjamas. Follow for more What Ifs!" })
$ids = @()
foreach ($l in $lines) {
  $ms = [int][math]::Round($l.at * 1000)
  $r = Call gen.create_clip @{ project = $p; model = 'omnivoice.bf16'; prompt = $l.say; at = "$($ms)@1000"; seconds = 3.2 }
  $ids += $r.clip
}
# The same voice for all of them, a little faster than normal: the energetic narrator of the brief.
$ops = New-Object System.Collections.ArrayList
foreach ($id in $ids) {
  $o = (& $bin\attome.exe --json get $p $id | ConvertFrom-Json).result.object
  [void]$ops.Add(@{ op = 'add'; path = "$id/media_ref/inputs/voice"; value = 'male, young adult, high pitch' })
  $node = ($o.media_ref.workflow.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq 'attome.generate_speech' }).Name
  [void]$ops.Add(@{ op = 'replace'; path = "$node/settings/speed"; value = 1.2 })
}
Call project.patch @{ project = $p; patch = @{ ops = @($ops); label = 'The narrator' } } | Out-Null
"clips: $($ids -join ' ')"
$ids | Out-File "$env:TEMP\voice_ids.txt" -Encoding utf8

