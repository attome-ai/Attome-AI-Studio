# UI test: captions (UX review U7). A voice clip that has been generated offers "Make captions": one text clip for each sentence on a
# Captions track, every word timed (content.words), drawn one word at a time. Editing a caption's text makes it a plain text again.
# Mock engine, virtual input only. Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_captions.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Captions.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

function First-Caption($run) {
  $caps = @(Get-Tracks $run) | Where-Object { $_.name -eq 'Captions' }
  if (-not $caps) { return $null }
  Get-Object $run @($caps.clip_list)[0].id
}

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    & "$bin\attome.exe" new $proj --rate 30 --canvas 360x640 | Out-Null
    [IO.File]::WriteAllText("$work\v.json", (@{ project = $proj; model = 'attome-mock-voice'; prompt = 'Hello there my friend. Free popcorn today!'; at = '0@1' } | ConvertTo-Json -Compress), (New-Object Text.UTF8Encoding($false)))
    $made = & "$bin\attome.exe" --json call gen.create_clip "$work\v.json" | ConvertFrom-Json
    if (-not $made.ok) { throw "setup: $($made.error.message)" }
    & "$bin\attome.exe" --json gen run $proj | Out-Null            # the mock says it: a tone as long as the words
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
    'wait 1500'
    'click @clip:Voice_1', 'wait 500'
    'expect @button:make_captions_pop'
    "shot $work\captions_card.jpg"
    'click @button:make_captions_pop', 'wait 1000'
    'expect @button:toast_undo'
    "shot $work\captions_made.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $caps = @(Get-Tracks $run) | Where-Object { $_.name -eq 'Captions' }
      if (-not $caps) { $failed = 'no Captions track was made' }
      else {
        $clips = @($caps.clip_list)
        "captions: $($clips.Count) clips"
        if ($clips.Count -ne 2) { $failed = "expected two sentences, found $($clips.Count) clips" }
        else {
          $first = First-Caption $run
          "first: '$($first.content.text)' with $(@($first.content.words).Count) words; outline: $($first.content.PSObject.Properties.Name -contains 'outline')"
          if (@($first.content.words).Count -ne 4) { $failed = 'the first sentence does not have four timed words' }
        }
      }
    }
    if (-not $failed) { # the voice is made again, twice as fast: the card offers to re-time, and the captions follow its words
      $before = [double](ConvertFrom-Rational (First-Caption $run | ForEach-Object { $_.timing.duration }))
      $v = @(Get-Tracks $run) | Where-Object { $_.kind -eq 'audio' } | Select-Object -First 1
      $vo = Get-Object $run $v.clip_list[0].id
      $node = ($vo.media_ref.workflow.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq 'attome.generate_speech' }).Name
      [IO.File]::WriteAllText("$work\sp.json", (@{ project = $proj; patch = @{ ops = @(@{ op = 'replace'; path = "$node/settings/speed"; value = 2.0 }); label = 'faster' } } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
      $null = Invoke-Attome $run --json call project.patch "$work\sp.json"
      $null = Invoke-Attome $run --json gen run $proj
      $sync = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('click @clip:Voice_1', 'wait 500', 'expect @button:sync_captions', 'click @button:sync_captions', 'wait 900')
      $failed = $sync.Errors
      if (-not $failed) {
        $after = [double](ConvertFrom-Rational (First-Caption $run | ForEach-Object { $_.timing.duration }))
        "first caption: $([math]::Round($before,2)) s before, $([math]::Round($after,2)) s after the voice got twice as fast"
        if ($after -gt $before * 0.7) { $failed = 'the captions did not follow the faster voice' }
      }
    }    if (-not $failed) { # editing the text makes it a plain text again
      $edit = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
        'click @clip:Caption_1', 'wait 500'
        'click @field:text_content', 'key A ctrl', 'type Bye now'
        'click @rail:Text', 'wait 700'
      )
      $failed = $edit.Errors
      if (-not $failed) {
        $first = First-Caption $run
        "after the edit: '$($first.content.text)', timed words: $($first.content.PSObject.Properties.Name -contains 'words')"
        if ($first.content.text -ne 'Bye now') { $failed = "the text is '$($first.content.text)', not 'Bye now'" }
        elseif ($first.content.PSObject.Properties.Name -contains 'words') { $failed = 'the words were kept after the text was edited' }
      }
    }
    if (-not $failed) {
      $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
      if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result.problems | ConvertTo-Json -Compress)" }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a generated voice makes word-by-word captions on a Captions track; editing a caption's text makes it a plain text (captures in $work)" -ForegroundColor Green
exit 0
