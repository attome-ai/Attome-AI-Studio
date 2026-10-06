param([Parameter(Mandatory)][string]$Project)
# Run from the repository root:  powershell -NoProfile -File <this script> -Project <path>.attome
$ErrorActionPreference = 'Stop'
$bin = "build\win-msvc-release\bin"  # the repo root is the working directory
$p = $Project
$sp = $PSScriptRoot
$py = if ($env:ATTOME_COMFY_PYTHON) { $env:ATTOME_COMFY_PYTHON } else { "C:\Users\Computia.me\Downloads\everything\attome-bench\ComfyUI_windows_portable\python_embeded\python.exe" }  # ComfyUI's python: has torch, kokoro, numpy, av
function Rat($s) { if ($s -match '^(-?\d+)/(\d+)$') { [double]$matches[1] / [double]$matches[2] } else { [double]$s } }
$ids = Get-Content "$env:TEMP\voice_ids.txt" | Where-Object { $_ }
$lines = @(); $starts = @(); $ends = @()
foreach ($id in $ids) {
  $o = (& $bin\attome.exe --json get $p $id | ConvertFrom-Json).result.object
  $node = ($o.media_ref.workflow.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq 'attome.generate_speech' }).Value
  $lines += @{ text = [string]$o.media_ref.inputs.text; voice = [string]$o.media_ref.inputs.voice; speed = [double]$node.settings.speed }
  $s = Rat $o.timing.record_in; $starts += $s; $ends += ($s + (Rat $o.timing.duration))
}
[IO.File]::WriteAllText("$env:TEMP\kk_lines.json", ($lines | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding($false)))
$ErrorActionPreference = 'Continue'; & $py "$sp\kokoro_align.py" "$env:TEMP\kk_lines.json" "$env:TEMP\kk_words.json" 2>$null; $ErrorActionPreference = 'Stop'
$al = Get-Content "$env:TEMP\kk_words.json" -Raw -Encoding utf8 | ConvertFrom-Json
$tracks = (& $bin\attome.exe --json inspect $p --level tracks | ConvertFrom-Json).result.data.sequences[0].tracks
$caps = @(($tracks | Where-Object { $_.name -eq 'Captions' }).clip_list)
$shad = @(($tracks | Where-Object { $_.name -eq 'Caption shadow' }).clip_list)
$ops = New-Object System.Collections.ArrayList
$w = 0
for ($i = 0; $i -lt $lines.Count; $i++) {
  $words = @($al[$i].words)
  $n = @($lines[$i].text -split '\s+' | Where-Object { $_ }).Count
  if ($words.Count -ne $n) { throw "line $($i+1): $($words.Count) timed words but $n caption words" }
  # what the voice clip really holds: the aligner's time scale vs the clip's length
  $scale = ($ends[$i] - $starts[$i]) / [double]$al[$i].length
  for ($k = 0; $k -lt $n; $k++) {
    $ws = $starts[$i] + $scale * [double]$words[$k].start
    $we = if ($k + 1 -lt $n) { $starts[$i] + $scale * [double]$words[$k + 1].start } else { [math]::Min($ends[$i], $starts[$i] + $scale * [double]$words[$k].end + 0.15) }
    $dur = [math]::Max([math]::Min(0.034, $we - $ws - 0.002), $we - $ws - 0.005)
    $in = "$([int][math]::Round($ws * 1000))/1000"; $du = "$([int][math]::Round($dur * 1000))/1000"
    foreach ($c in @($caps[$w], $shad[$w])) {
      [void]$ops.Add(@{ op = 'replace'; path = "$($c.id)/timing/record_in"; value = $in })
      [void]$ops.Add(@{ op = 'replace'; path = "$($c.id)/timing/duration"; value = $du })
    }
    $w++
  }
}
"$w words timed from the voice"
$f = "$env:TEMP\sync.json"
[IO.File]::WriteAllText($f, (@{ project = $p; patch = @{ ops = @($ops); label = 'Captions timed by the voice' } } | ConvertTo-Json -Depth 8 -Compress), (New-Object Text.UTF8Encoding($false)))
$r = & $bin\attome.exe --json call project.patch $f | ConvertFrom-Json
"ok=$($r.ok) $($r.error.message) $($r.error.data.hint) $(($r.error.data.errors | Select-Object -First 1 | ConvertTo-Json -Compress))"



