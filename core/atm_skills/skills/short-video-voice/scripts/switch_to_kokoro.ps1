param([Parameter(Mandatory)][string]$Project, [string]$Voice = 'am_michael')
# Run from the repository root:  powershell -NoProfile -File <this script> -Project <path>.attome
$ErrorActionPreference = 'Stop'
$bin = "build\win-msvc-release\bin"  # the repo root is the working directory
$p = $Project
$voice = if ($args[0]) { $args[0] } else { 'am_michael' }
function Rat($s) { if ($s -match '^(-?\d+)/(\d+)$') { [double]$matches[1] / [double]$matches[2] } else { [double]$s } }
function Patch($ops, $label) {
  $f = "$env:TEMP\kk.json"
  [IO.File]::WriteAllText($f, (@{ project = $p; patch = @{ ops = @($ops); label = $label } } | ConvertTo-Json -Depth 8 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & $bin\attome.exe --json call project.patch $f | ConvertFrom-Json
  if (-not $r.ok) { throw "patch: $($r.error.message) $($r.error.data.hint) $($r.error.data.errors | ConvertTo-Json -Compress)" }
}
$ids = Get-Content "$env:TEMP\voice_ids.txt" | Where-Object { $_ }
$ops = New-Object System.Collections.ArrayList
foreach ($id in $ids) {
  $o = (& $bin\attome.exe --json get $p $id | ConvertFrom-Json).result.object
  $node = ($o.media_ref.workflow.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq 'attome.generate_speech' }).Name
  [void]$ops.Add(@{ op = 'replace'; path = "$node/model"; value = 'kokoro.82m' })
  [void]$ops.Add(@{ op = 'replace'; path = "$node/settings"; value = @{ speed = 1.0 } })
  [void]$ops.Add(@{ op = 'replace'; path = "$id/media_ref/inputs/voice"; value = $voice })
}
Patch $ops "Narrator: Kokoro $voice"
$r = & $bin\attome.exe --json gen run $p | Out-String
"run: " + $r.Substring(0, [math]::Min(300, $r.Length))
$parts = @(); $prevEnd = 0; $fix = New-Object System.Collections.ArrayList
foreach ($id in $ids) {
  $o = (& $bin\attome.exe --json get $p $id | ConvertFrom-Json).result.object
  $s = Rat $o.timing.record_in; $d = Rat $o.timing.duration
  if ($s -lt $prevEnd + 0.05) { $s = $prevEnd + 0.1; [void]$fix.Add(@{ op = 'replace'; path = "$id/timing/record_in"; value = "$([int][math]::Round($s * 1000))/1000" }) }
  $prevEnd = $s + $d
  $parts += "[$s,$($s + $d)]"
  "$($o.name) $([math]::Round($s,2)) - $([math]::Round($s + $d,2)) ($([math]::Round($d,2)) s)"
}
if ($fix.Count) { Patch $fix 'No overlap'; "pushed $($fix.Count)" }
[IO.File]::WriteAllText("$env:TEMP\voices.json", "[" + ($parts -join ',') + "]")

