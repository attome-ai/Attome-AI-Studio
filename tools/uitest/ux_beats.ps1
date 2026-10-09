# UI test: beat marks on music (UX review 4, P6). A music clip's menu has "Show beats": the beat of its sound is found (in the
# background) and marked on the clip, and the clip's audio.beats is saved; "Hide beats" takes it away. A sound with no beat (a steady
# tone) says so. A clip dragged near a beat catches on it. Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_beats.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Beats.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

# A WAV, 48 kHz stereo 16 bit, of `seconds`: clicks at 120 bpm (a short burst every half second from 0.25 s), or a steady 440 Hz tone.
function Write-Wav([string]$path, [double]$seconds, [bool]$clicks) {
  $rate = 48000; $frames = [int]($seconds * $rate)
  $data = New-Object byte[] ($frames * 4)
  $put = { param($at, $v) $o = $at * 4; $lo = [byte]($v -band 0xFF); $hi = [byte](($v -shr 8) -band 0xFF); $data[$o] = $lo; $data[$o + 1] = $hi; $data[$o + 2] = $lo; $data[$o + 3] = $hi }
  if ($clicks) {
    for ($b = 0; 0.25 + 0.5 * $b -lt $seconds - 0.1; $b++) {
      $at = [int]((0.25 + 0.5 * $b) * $rate)
      for ($i = 0; $i -lt 1200; $i++) { & $put ($at + $i) ([int16]([math]::Round(20000 * [math]::Exp(-$i / 200.0) * [math]::Sin(2 * [math]::PI * 1000 * $i / $rate)))) }
    }
  } else {
    for ($i = 0; $i -lt $frames; $i++) { & $put $i ([int16]([math]::Round(8000 * [math]::Sin(2 * [math]::PI * 440 * $i / $rate)))) }
  }
  $s = [IO.File]::Create($path); $w = New-Object IO.BinaryWriter($s)
  $w.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $w.Write([int](36 + $data.Length)); $w.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
  $w.Write([int]16); $w.Write([int16]1); $w.Write([int16]2); $w.Write([int]$rate); $w.Write([int]($rate * 4)); $w.Write([int16]4); $w.Write([int16]16)
  $w.Write([Text.Encoding]::ASCII.GetBytes('data')); $w.Write([int]$data.Length); $w.Write($data); $w.Close()
}

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  Write-Wav "$work\clicks.wav" 8 $true
  Write-Wav "$work\tone.wav" 4 $false
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(
        @{ op = 'add_clip'; path = "$work\clicks.wav"; name = 'clicks'; at = '0s' },
        @{ op = 'add_clip'; path = "$work\tone.wav"; name = 'tone'; at = '0s'; track = 'new' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message) $($r.error.hint)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Clip($run, [string]$name) { foreach ($t in @(Get-Tracks $run)) { foreach ($c in @($t.clip_list)) { if ($c.name -eq $name) { return Get-Object $run $c.id } } } }

$failed = $null
$run = Invoke-EditorScript -Project $proj -TimeoutSeconds 200 -Script @(
  'wait 1200'
  'absent @beats:clicks'
  'rclick @clip:clicks', 'wait 400', 'click @menuitem:Show_beats', 'wait 2500'
  'expect @beats:clicks'
  "shot $work\beats_shown.jpg"
  'rclick @clip:tone', 'wait 400', 'click @menuitem:Show_beats', 'wait 2500'
  'absent @beats:tone'
  "shot $work\no_beat.jpg"
)
try {
  $failed = $run.Errors
  if (-not $failed -and (Clip $run 'clicks').audio.beats -ne $true) { $failed = 'Show beats did not save audio.beats on the clip' }
  if (-not $failed) { # saved: a new editor shows them at once; Hide beats takes them away
    # and a clip dragged near a beat catches on it: the tone moved about 0.7 s lands on the beat at 0.75 s
    $h = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('wait 2500', 'expect @beats:clicks', 'drag @clip:tone 64 0', 'wait 800',
      'rclick @clip:clicks', 'wait 400', 'click @menuitem:Hide_beats', 'wait 800', 'absent @beats:clicks')
    $failed = $h.Errors
    if (-not $failed) {
      $at = ConvertFrom-Rational (Clip $run 'tone').timing.record_in
      "the tone starts at $at s"
      if ([math]::Abs($at - 0.75) -gt 0.02) { $failed = "the tone dragged near the beat at 0.75 s starts at $at s" }
    }
    if (-not $failed -and (Clip $run 'clicks').audio.beats) { $failed = 'Hide beats left audio.beats on the clip' }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Show beats marks the beat of a music clip and is saved, Hide beats takes it away, a tone has none (captures in $work)" -ForegroundColor Green
exit 0
