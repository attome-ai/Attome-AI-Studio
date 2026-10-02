# Runs every UI test. Virtual input only: the computer stays usable meanwhile. Exit code 0 = all passed.
#   .\tools\uitest\run_all.ps1

$failed = @()
foreach ($test in Get-ChildItem $PSScriptRoot -Filter *.ps1 | Where-Object { $_.Name -ne 'run_all.ps1' }) {
  $out = & $test.FullName 6>&1 2>&1
  $line = ($out | Where-Object { "$_" -match '^(PASS|FAIL)' } | Select-Object -Last 1)
  if ($LASTEXITCODE -ne 0 -or "$line" -notmatch '^PASS') { $failed += $test.BaseName }
  "{0,-10} {1}" -f $test.BaseName, $(if ($line) { "$line" } else { ($out | Select-Object -Last 1) })
}
if ($failed) { Write-Host "FAILED: $($failed -join ', ')" -ForegroundColor Red; exit 1 }
Write-Host 'All UI tests passed.' -ForegroundColor Green
