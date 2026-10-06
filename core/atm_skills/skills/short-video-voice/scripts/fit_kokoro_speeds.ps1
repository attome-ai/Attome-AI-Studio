param([Parameter(Mandatory)][string]$Project)
# Run from the repository root:  powershell -NoProfile -File <this script> -Project <path>.attome
$ErrorActionPreference = 'Stop'
$bin = "build\win-msvc-release\bin"  # the repo root is the working directory
$p = $Project
function Rat($s) { if ($s -match '^(-?\d+)/(\d+)$') { [double]$matches[1] / [double]$matches[2] } else { [double]$s } }
function Patch($ops, $label) {
  $f = "$env:TEMP\kk.json"
  [IO.File]::WriteAllText($f, (@{ project = $p; patch = @{ ops = @($ops); label = $label } } | ConvertTo-Json -Depth 8 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & $bin\attome.exe --json call project.patch $f | ConvertFrom-Json
  if (-not $r.ok) { throw "patch: $($r.error.message) $($r.error.data.hint) $($r.error.data.errors | ConvertTo-Json -Compress)" }
}
$ids = Get-Content "$env:TEMP\voice_ids.txt" | Where-Object { $_ }
$scene = @(0.15, 3.1, 7.1, 11.1, 15.1, 19.1, 23.1, 27.1, 31.1)
$budget = 3.75
$ops = New-Object System.Collections.ArrayList
$i = 0
foreach ($id in $ids) {
  $o = (& $bin\attome.exe --json get $p $id | ConvertFrom-Json).result.object
  $node = ($o.media_ref.workflow.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq 'attome.generate_speech' }).Name
  $cur = [double]$o.media_ref.workflow.nodes.$node.settings.speed
  $d = Rat $o.timing.duration
  $natural = $d * $cur                                  # the length at speed 1.0
  $b = if ($i -eq 0) { 3.5 } else { $budget }
  $speed = [math]::Round([math]::Min(1.4, [math]::Max(1.0, $natural / $b)), 2)
  [void]$ops.Add(@{ op = 'replace'; path = "$node/settings/speed"; value = $speed })
  "$($o.name): natural $([math]::Round($natural,2)) s -> speed $speed"
  $i++
}
Patch $ops 'Kokoro speeds to fit the scenes'
& $bin\attome.exe --json gen run $p | Out-Null
$parts = @(); $prevEnd = 0; $fix = New-Object System.Collections.ArrayList; $k = 0
foreach ($id in $ids) {
  $o = (& $bin\attome.exe --json get $p $id | ConvertFrom-Json).result.object
  $s = $scene[$k]; $k++; $d = Rat $o.timing.duration
  if ($s -lt $prevEnd + 0.05) { $s = $prevEnd + 0.1 }
  [void]$fix.Add(@{ op = 'replace'; path = "$id/timing/record_in"; value = "$([int][math]::Round($s * 1000))/1000" })
  $prevEnd = $s + $d
  $parts += "[$s,$($s + $d)]"
  "$($o.name) $([math]::Round($s,2)) - $([math]::Round($s + $d,2)) ($([math]::Round($d,2)) s)"
}
if ($fix.Count) { Patch $fix 'No overlap'; "pushed $($fix.Count)" }
[IO.File]::WriteAllText("$env:TEMP\voices.json", "[" + ($parts -join ',') + "]")


