param([Parameter(Mandatory)][string]$Project)
# Run from the repository root:  powershell -NoProfile -File <this script> -Project <path>.attome
$ErrorActionPreference = 'Stop'
$bin = "build\win-msvc-release\bin"  # the repo root is the working directory
$p = $Project
function Call($tool, $o) {
  $f = "$env:TEMP\call7.json"
  [IO.File]::WriteAllText($f, ($o | ConvertTo-Json -Depth 20 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & $bin\attome.exe --json call $tool $f | ConvertFrom-Json
  if (-not $r.ok) { throw "$tool failed: $($r.error.message) $($r.error.hint)" }
  $r.result
}
$proj = (& $bin\attome.exe --json inspect $p | ConvertFrom-Json).result.data
$assets = (& $bin\attome.exe --json get $p $proj.id | ConvertFrom-Json).result.object.assets
$asset = ($assets.PSObject.Properties | Where-Object { $_.Value.name -eq 'skeleton_beat.wav' } | Select -First 1).Name
"asset $asset"
$tracks = (& $bin\attome.exe --json inspect $p --level tracks | ConvertFrom-Json).result.data.sequences[0].tracks
$v1 = $tracks | Where-Object { $_.name -eq 'V1' }
$o = (& $bin\attome.exe --json get $p $v1.id | ConvertFrom-Json).result.object

# 1. The sound the scenes came with is muted.
$ops = @()
foreach ($c in $o.clip_order) { $ops += @{ op = ($(if ($o.clips.$c.PSObject.Properties.Name -contains 'volume') { 'replace' } else { 'add' })); path = "$c/volume"; value = 0 } }
$r = Call project.patch @{ project = $p; patch = @{ ops = $ops; label = 'Mute the generated sound' } }
"muted $($ops.Count) scenes, revision $($r.revision)"

# 2. The music, on an audio track, under the whole film.
$r = Call timeline.edit @{ project = $p; label = 'Add the music'; ops = @(@{ op = 'add_clip'; asset = $asset; at = '0@30'; name = 'Skeleton beat' }) }
$r | ConvertTo-Json -Depth 4 -Compress

