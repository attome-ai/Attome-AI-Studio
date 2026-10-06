param([Parameter(Mandatory)][string]$Project)
# Run from the repository root:  powershell -NoProfile -File <this script> -Project <path>.attome
$ErrorActionPreference = 'Stop'
$bin = "build\win-msvc-release\bin"  # the repo root is the working directory
$p = $Project
$py = if ($env:ATTOME_COMFY_PYTHON) { $env:ATTOME_COMFY_PYTHON } else { "C:\Users\Computia.me\Downloads\everything\attome-bench\ComfyUI_windows_portable\python_embeded\python.exe" }  # ComfyUI's python: has torch, kokoro, numpy, av
function Rat($s) { if ($s -match '^(-?\d+)/(\d+)$') { [double]$matches[1] / [double]$matches[2] } else { [double]$s } }
$wavs = Get-ChildItem "$p\.attome\gen" -Recurse -Filter audio.wav | Sort-Object LastWriteTime -Descending | Select-Object -First 9
$peaks = @{}
foreach ($w in $wavs) { $peaks[$w.FullName] = [double](& $py -c "import sys,soundfile as sf,numpy as np;a,sr=sf.read(sys.argv[1]);print(float(np.abs(a).max()))" $w.FullName) }
$ids = Get-Content "$env:TEMP\voice_ids.txt" | Where-Object { $_ }
$ops = New-Object System.Collections.ArrayList
foreach ($id in $ids) {
  $o = (& $bin\attome.exe --json get $p $id | ConvertFrom-Json).result.object
  $d = Rat $o.timing.duration
  $w = $wavs | Sort-Object { [math]::Abs((($_.Length - 44) / 192000.0) - $d) } | Select-Object -First 1
  $db = [math]::Round([math]::Min(10.0, 20 * [math]::Log10(0.9 / $peaks[$w.FullName])), 1)
  $op = if ($o.PSObject.Properties.Name -contains 'audio') { 'replace' } else { 'add' }
  $val = if ($op -eq 'add') { @{ gain_db = $db } } else { $db }
  $path = if ($op -eq 'add') { "$id/audio" } else { "$id/audio/gain_db" }
  [void]$ops.Add(@{ op = $op; path = $path; value = $val })
  "$($o.name): peak $([math]::Round($peaks[$w.FullName],2)) -> +$db dB"
}
$f = "$env:TEMP\loud.json"
[IO.File]::WriteAllText($f, (@{ project = $p; patch = @{ ops = @($ops); label = 'Voice louder' } } | ConvertTo-Json -Depth 8 -Compress), (New-Object Text.UTF8Encoding($false)))
$r = & $bin\attome.exe --json call project.patch $f | ConvertFrom-Json
"ok=$($r.ok) $($r.error.message) $($r.error.data.hint)"

