# UI test: speech. The Generate panel has an Audio tab with the voice model; a click adds a Voice clip on an audio track (not on the
# picture track); its text is typed on its card; Generate makes the speech and the clip becomes as long as what was said; dragging
# the voice card onto the timeline lands on an audio track. Mock engine (a tone as long as the words), virtual input only.
#   .\tools\uitest\generate_voice.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Voice.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try { & "$bin\attome.exe" new $proj --rate 24 --canvas 640x352 | Out-Null } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -TimeoutSeconds 240 -Script @(
    'click @rail:Generate'
    'wait 400'
    'click @tab:Audio'
    'expect @model:attome-mock-voice'
    "shot $work\voice_panel.jpg"             # the voice card, with the bars of a waveform
    'click @model:attome-mock-voice'         # a click adds it at the end of an audio track
    'wait 600'
    'expect @input:text'
    'click @field:input_text'
    'type Hour one: you vanish! Free popcorn, no ticket, no problem.'
    'click @rail:Generate'                   # leaving the field saves the text
    'wait 400'
    "shot $work\voice_card.jpg"
    'click @button:gen_run'
    'wait 300'
    'expect @button:gen_run'
    'wait 1500'
    "shot $work\voice_done.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $tracks = Get-Tracks $run
      $info = ($tracks | ForEach-Object { "$($_.name)/$($_.kind)=$(@($_.clip_list).Count)" }) -join ', '
      "tracks: $info"
      $voice = $tracks | Where-Object { @($_.clip_list).Count -gt 0 } | Select-Object -First 1
      $clip = Get-Object $run $voice.clip_list[0].id
      $len = ConvertFrom-Rational $clip.timing.duration
      "clip '$($clip.name)' on a $($voice.kind) track, $len s; text: $($clip.media_ref.inputs.text)"
      if ($voice.kind -ne 'audio') { $failed = 'the voice is not on an audio track' }
      elseif ($clip.name -ne 'Voice 1') { $failed = "the clip is named '$($clip.name)', not Voice 1" }
      elseif ($clip.media_ref.inputs.text -notmatch 'Hour one') { $failed = 'the text was not saved on the clip' }
      elseif ([math]::Abs($len - 4.0) -gt 0.12) { $failed = "the clip is $len s: 10 words at 0.4 s should make it 4.0 s" }
      else {
        $st = (Invoke-Attome $run --json gen status $proj | ConvertFrom-Json).result.clips[0]
        if ($st.state -ne 'clean' -or $st.takes -ne 1) { $failed = "the clip is $($st.state) with $($st.takes) Takes: it was not generated" }
      }
    }
    if (-not $failed) { # the card dragged onto the timeline: an audio track, below the first
      $more = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -TimeoutSeconds 240 -Script @(
        'click @rail:Generate'
        'wait 300'
        'click @tab:Audio'
        'drag @model:attome-mock-voice @clip:Voice_1@0.5,1.5'
        'wait 800'
        "shot $work\voice_dropped.jpg"
      )
      $failed = $more.Errors
      if (-not $failed) {
        $tracks = Get-Tracks $run
        $audio = @($tracks | Where-Object { $_.kind -eq 'audio' })
        $count = ($audio | ForEach-Object { @($_.clip_list).Count } | Measure-Object -Sum).Sum
        "audio tracks: $($audio.Count) with $count clips"
        if ($count -ne 2) { $failed = "expected 2 voice clips on audio tracks, found $count" }
        else {
          $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
          if (-not $valid.result.ok) { $failed = 'the project is not valid' }
        }
      }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a voice model on the Audio tab makes a Voice clip on an audio track, as long as what it says (captures in $work)" -ForegroundColor Green
exit 0
