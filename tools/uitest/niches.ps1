# UI test: the Niches panel (plan docs/plan/NICHES.md, N5). The rail has Niches; the list shows the built-in niches; a built-in one is opened as
# text, changed and kept as the project's own (a skill with the description "Niche: ..."); "Learn from a video" writes a draft niche from
# video.analyze, which is kept and then deleted. Virtual input only (uitest.psm1). Look at the captures too.
#   .\tools\uitest\niches.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Niches.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue
$ep = "\\.\pipe\attome-uitest-niches-$PID"

# a small video with known cuts, made by the engine itself
$env:ATTOME_ENDPOINT = $ep
try {
  & "$bin\attome.exe" new $proj --rate 30 --canvas 640x360 | Out-Null
  $video = Join-Path $work 'learn.mp4'
  New-Sample $video '--seconds 5 --height 360'
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Get-ProjectNiches($run) {
  $p = Join-Path $work 'skills.json'
  [IO.File]::WriteAllText($p, (@{ project = $proj } | ConvertTo-Json -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = Invoke-Attome $run --json call skill.list $p | ConvertFrom-Json
  @($r.result.skills | Where-Object { $_.source -eq 'project' })
}

# 1. the list, a built-in niche as text, one more line, kept as the project's own
$steps = @('wait 1200', 'click @rail:Niches', 'wait 400', 'expect @niche:niche-funny-short', 'expect @niche:niche-skeleton',
           "shot $work\list.jpg", 'click @niche:niche-funny-short', 'wait 400', "shot $work\default_text.jpg",
           'click @field:niche_text', 'key End ctrl', 'type  ZZZ-MY-LINE', 'wait 200', 'click @button:niche_save', 'wait 600', "shot $work\saved.jpg")
$run = Invoke-EditorScript -Project $proj -Endpoint $ep -TimeoutSeconds 240 -Script $steps
$failed = $run.Errors
if (-not $failed) {
  $mine = @(Get-ProjectNiches $run)
  "after the first run: $($mine.Count) niche(s) of the project: $($mine.title -join ', ')"
  if ($mine.Count -ne 1) { $failed = "expected 1 niche kept in the project, found $($mine.Count)" }
  else {
    $p = Join-Path $work 'get.json'
    [IO.File]::WriteAllText($p, (@{ project = $proj; id = $mine[0].id } | ConvertTo-Json -Compress), (New-Object Text.UTF8Encoding($false)))
    $body = (Invoke-Attome $run --json call skill.get $p | ConvertFrom-Json).result.body
    if ($body -notmatch 'ZZZ-MY-LINE') { $failed = 'the line typed into the niche was not kept' }
    elseif ($body -notmatch 'CHECKS') { $failed = 'the built-in niche text was not the starting point' }
  }
}

# 2. learn from a video: the draft is made, kept, then deleted
if (-not $failed) {
  $env:ATTOME_EDITOR_PICK_NICHE_VIDEO = $video
  try {
    $steps = @('wait 1200', 'click @rail:Niches', 'wait 400', 'click @button:niche_learn', 'wait 1500', "shot $work\draft.jpg",
               'click @button:niche_save', 'wait 600')
    $run2 = Invoke-EditorScript -Project $proj -Endpoint $ep -TimeoutSeconds 240 -Script $steps
  } finally { Remove-Item Env:\ATTOME_EDITOR_PICK_NICHE_VIDEO -ErrorAction SilentlyContinue }
  $failed = $run2.Errors
  if (-not $failed) {
    $mine = @(Get-ProjectNiches $run)
    "after learning: $($mine.Count) niche(s): $($mine.title -join ' | ')"
    if ($mine.Count -ne 2) { $failed = "expected 2 niches, found $($mine.Count)" }
    elseif (-not ($mine.title -match 'Learned from learn')) { $failed = 'the learned niche has no name from the video' }
  }
}

# 3. delete the user's niches by key: open each in the list and press Delete
if (-not $failed) {
  $learned = Get-ProjectNiches $run | Where-Object { $_.title -match 'Learned from' } | Select-Object -First 1
  $steps = @('wait 1200', 'click @rail:Niches', 'wait 400', "click @niche:$($learned.id)", 'wait 400', "shot $work\learned.jpg",
             'click @button:niche_delete', 'wait 600', "shot $work\deleted.jpg")
  $run3 = Invoke-EditorScript -Project $proj -Endpoint $ep -TimeoutSeconds 240 -Script $steps
  $failed = $run3.Errors
  if (-not $failed) {
    $left = @(Get-ProjectNiches $run)
    if ($left.Count -ne 1) { $failed = "expected 1 niche after the delete, found $($left.Count)" }
  }
}
# 4. a link: with the optional downloader present (a fake one here: a script that copies a sample video to the folder it is given), the video
#    is "fetched", measured and deleted, and the draft is kept like any other
if (-not $failed) {
  Stop-Daemon $run
  $fakeDir = Join-Path $work 'fake'
  New-Item -ItemType Directory -Force $fakeDir | Out-Null
  # a tiny program standing in for yt-dlp: copies the sample to the folder it is given (-P) and writes the --print-to-file targets
  $source = @'
using System; using System.IO; using System.Collections.Generic;
public static class FakeYtDlp {
  public static int Main(string[] a) {
    string folder = null; var targets = new List<string[]>();
    for (int i = 0; i < a.Length; i++) {
      if (a[i] == "-P") folder = a[i + 1];
      if (a[i] == "--print-to-file") targets.Add(new[] { a[i + 1], a[i + 2] });
    }
    string file = Path.Combine(folder, "fake.mp4");
    File.Copy(Environment.GetEnvironmentVariable("FAKE_YTDLP_SAMPLE"), file, true);
    foreach (var t in targets) File.WriteAllText(t[1], t[0].Contains("filepath") ? file : "Fake title");
    Console.WriteLine("[download]  50.0% of 1.00MiB");
    return 0;
  }
}
'@
  if (-not (Test-Path "$fakeDir\yt-dlp.exe")) { Add-Type -TypeDefinition $source -OutputAssembly "$fakeDir\yt-dlp.exe" -OutputType ConsoleApplication }
  $saved = @{ ATTOME_YTDLP = $env:ATTOME_YTDLP; FAKE_YTDLP_SAMPLE = $env:FAKE_YTDLP_SAMPLE; ATTOME_MODELS_DIR = $env:ATTOME_MODELS_DIR }
  $env:ATTOME_YTDLP = "$fakeDir\yt-dlp.exe"; $env:FAKE_YTDLP_SAMPLE = $video; $env:ATTOME_MODELS_DIR = "$work\appdata\models"
  $ep4 = "\\.\pipe\attome-uitest-niches4-$PID"
  try {
    $steps = @('wait 1200', 'click @rail:Niches', 'wait 400', 'click @field:niche_link', 'type https://example.com/watch?v=abc', 'wait 200',
               "shot $work\link.jpg", 'click @button:niche_get_link', 'wait 5000', "shot $work\link_done.jpg", 'click @button:niche_save', 'wait 600')
    $run4 = Invoke-EditorScript -Project $proj -Endpoint $ep4 -TimeoutSeconds 240 -Script $steps
  } finally { foreach ($k in $saved.Keys) { if ($saved[$k]) { Set-Item "Env:\$k" $saved[$k] } else { Remove-Item "Env:\$k" -ErrorAction SilentlyContinue } } }
  $failed = $run4.Errors
  if (-not $failed) {
    $mine = @(Get-ProjectNiches $run4)
    "after the link: $($mine.Count) niche(s): $($mine.title -join ' | ')"
    if (-not ($mine.title -match 'Learned from Fake title')) { $failed = 'the niche learned from the link was not kept under the video title' }
    elseif (Get-ChildItem "$work\appdata" -Recurse -Filter 'fake.mp4' -ErrorAction SilentlyContinue) { $failed = 'the downloaded video was not deleted after it was measured' }
  }
  Stop-Daemon $run4
}

# 5. "Also keep this video's sound": checked, a local "Learn from a video" also imports the sound as the project's own media asset
if (-not $failed) {
  $steps = @('wait 1200', 'click @rail:Niches', 'wait 400', 'click @check:niche_keep_sound', 'wait 200', 'click @button:niche_learn', 'wait 1500',
             "shot $work\with_sound.jpg", 'wait 600')
  $env:ATTOME_EDITOR_PICK_NICHE_VIDEO = $video
  try { $run5 = Invoke-EditorScript -Project $proj -Endpoint $ep -TimeoutSeconds 240 -Script $steps }
  finally { Remove-Item Env:\ATTOME_EDITOR_PICK_NICHE_VIDEO -ErrorAction SilentlyContinue }
  $failed = $run5.Errors
  if (-not $failed) {
    $info = (Invoke-Attome $run5 --json inspect $proj | ConvertFrom-Json).result.data
    $obj = (Invoke-Attome $run5 --json get $proj $info.id | ConvertFrom-Json).result.object
    $assets = @($obj.assets.PSObject.Properties)
    "assets after keeping the sound: $($assets.Count)"
    if ($assets.Count -lt 1) { $failed = "the video's sound was not imported into the project's media" }
    elseif (-not ($assets.Value.name -match '\.wav$')) { $failed = "the imported asset is not a sound file" }
  }
  Stop-Daemon $run5
}
Stop-Daemon $run
if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: the Niches panel lists, edits, keeps, learns from a video and deletes; look at the captures in $work" -ForegroundColor Green
