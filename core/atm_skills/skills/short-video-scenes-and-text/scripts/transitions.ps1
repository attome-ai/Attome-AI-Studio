param([Parameter(Mandatory)][string]$Project)
# Run from the repository root:  powershell -NoProfile -File <this script> -Project <path>.attome
$ErrorActionPreference = 'Stop'
$bin = "build\win-msvc-release\bin"  # the repo root is the working directory
$p = $Project
$tracks = (& $bin\attome.exe --json inspect $p --level tracks | ConvertFrom-Json).result.data.sequences[0].tracks
$v1 = $tracks | Where-Object { $_.name -eq 'V1' }
$o = (& $bin\attome.exe --json get $p $v1.id | ConvertFrom-Json).result.object
$ids = @($o.clip_order)
# One transition at each of the eight cuts; a different one each time.
$kinds = @(
  @{ type = 'attome.zoom';     params = @{ amount = 0.6; direction = 'in' } },
  @{ type = 'attome.push';     params = @{ direction = 'left' } },
  @{ type = 'attome.wipe';     params = @{ direction = 'down'; softness = 0.15 } },
  @{ type = 'attome.zoom';     params = @{ amount = 0.6; direction = 'in' } },
  @{ type = 'attome.slide';    params = @{ direction = 'left' } },
  @{ type = 'attome.iris';     params = @{ softness = 0.2 } },
  @{ type = 'attome.zoom';     params = @{ amount = 0.5; direction = 'out' } },
  @{ type = 'attome.zoom';     params = @{ amount = 0.7; direction = 'in' } })
$ops = @()
for ($i = 0; $i -lt 8; ++$i) {
  $ops += @{ op = 'add'; path = "$($v1.id)/transitions/`$new:t$i"; value = @{
      type = $kinds[$i].type; from = $ids[$i]; to = $ids[$i + 1]; in_offset = '0@30'; out_offset = '10@30'; params = $kinds[$i].params } }
}
$f = "$env:TEMP\trans.json"
[IO.File]::WriteAllText($f, (@{ project = $p; patch = @{ ops = $ops; label = 'Transitions' } } | ConvertTo-Json -Depth 8 -Compress), (New-Object Text.UTF8Encoding($false)))
$r = & $bin\attome.exe --json call project.patch $f | ConvertFrom-Json
if (-not $r.ok) { "FAILED: $($r.error.message) | $($r.error.hint) | $($r.error.path)"; exit 1 } else { "revision $($r.result.revision)" }
& $bin\attome.exe --json validate $p | ConvertFrom-Json | ForEach-Object { "valid=$($_.result.ok) warnings=$(@($_.result.warnings).Count)" }

