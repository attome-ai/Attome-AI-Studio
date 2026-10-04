# Runs every UI test, several at a time (about 10 s for all 20 with 8 at a time; 60 s one after the other). Virtual input only, and the test windows sit off-screen (ATTOME_EDITOR_SHOW=1 puts
# them on screen), so the computer stays usable meanwhile. Each test has a work folder of its own (%TEMP%\attome-uitest\<test>),
# which is also where its captures go. Exit code 0 = all passed.
#   .\tools\uitest\run_all.ps1            # 8 at a time
#   .\tools\uitest\run_all.ps1 -Jobs 1    # one after the other
#   .\tools\uitest\run_all.ps1 chroma_key lut   # only these

param([int]$Jobs = 8, [Parameter(ValueFromRemainingArguments)][string[]]$Only)

$clock = [Diagnostics.Stopwatch]::StartNew()
$tests = Get-ChildItem $PSScriptRoot -Filter *.ps1 | Where-Object { $_.Name -ne 'run_all.ps1' -and (-not $Only -or $Only -contains $_.BaseName) }
$running = @()
$started = @{}
$results = @()
# The slowest tests first: the run ends when the last one does, so they should not wait for a free slot.
$slow = 'grade_wipe', 'make_room', 'chroma_pick', 'slide_iris', 'track_lock', 'transform', 'effect_keys'
$queue = [System.Collections.Queue]::new(@($tests | Sort-Object { $i = [array]::IndexOf($slow, $_.BaseName); if ($i -ge 0) { $i } else { 99 } }))

function Receive-Finished($job) {
  $out = Receive-Job $job 6>&1 2>&1
  $line = ($out | Where-Object { "$_" -match '^(PASS|FAIL)' } | Select-Object -Last 1)
  $ok = "$line" -match '^PASS'
  [pscustomobject]@{ Test = $job.Name; Ok = $ok; Line = $(if ($line) { "$line" } else { "$($out | Select-Object -Last 1)" }) }
}

while ($queue.Count -gt 0 -or $running.Count -gt 0) {
  while ($queue.Count -gt 0 -and $running.Count -lt $Jobs) {
    $t = $queue.Dequeue()
    $started[$t.BaseName] = $clock.Elapsed.TotalSeconds
    $running += Start-Job -Name $t.BaseName -ScriptBlock { param($path) & $path *>&1 | ForEach-Object { "$_" } } -ArgumentList $t.FullName
  }
  $done = @($running | Where-Object { $_.State -ne 'Running' })
  if (-not $done) { Start-Sleep -Milliseconds 200; continue }
  foreach ($job in $done) {
    $r = Receive-Finished $job
    $results += $r
    $took = $clock.Elapsed.TotalSeconds - $started[$r.Test]
    "{0,-14} {1,5:N1} s  {2}" -f $r.Test, $took, $r.Line
    Remove-Job $job
  }
  $running = @($running | Where-Object { $done -notcontains $_ }) # only those handled above: one that finished meanwhile is next time's
}

if ($results.Count -ne @($tests).Count) { # never report a pass for a test that did not report
  Write-Host "Only $($results.Count) of $(@($tests).Count) tests reported." -ForegroundColor Red
  exit 1
}
$failed = @($results | Where-Object { -not $_.Ok } | ForEach-Object { $_.Test })
"{0} tests in {1:N0} s" -f $results.Count, $clock.Elapsed.TotalSeconds
if ($failed) { Write-Host "FAILED: $($failed -join ', ')" -ForegroundColor Red; exit 1 }
Write-Host 'All UI tests passed.' -ForegroundColor Green
