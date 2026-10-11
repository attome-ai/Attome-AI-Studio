# UI test: the MiniMax H3 controls on a generative clip's workflow card. A second clip is set, in the Inspector, to carry on from the clip
# before ("Continues": the clip then skips the 22 frames that repeat the first clip's end and a Clip Reference node feeds the node's
# continue_from), to the Final quality, and to Low memory; the clip's own workflow holds each of them. Mock engine: no model, no GPU.
# Virtual input only (uitest.psm1). Exit code 0 = pass.
#   .\tools\uitest\h3_pro.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'H3.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_MOCK_ENGINE = '1'
$env:ATTOME_MODELS_DIR = Join-Path $work 'models'
$h3 = 'minimax-h3.fl2va.turbo8-int8'
try {
  $env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
  try {
    & "$bin\attome.exe" new $proj --rate 24 --canvas 480x832 | Out-Null
    foreach ($n in 1, 2) {
      $p = Join-Path $work "clip$n.json"
      @{ project = $proj; prompt = "Scene $n"; model = $h3; seconds = 5 } | ConvertTo-Json | Set-Content $p -Encoding utf8
      & "$bin\attome.exe" --daemon never --json call gen.create_clip $p | Out-Null
    }
  } finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

  $run = Invoke-EditorScript -Project $proj -Script @(
    'wait 600'
    'click @clip:Shot_2'                    # its card is in the Inspector
    'wait 400'
    'expect @combo:gen_continue'            # an H3 clip has the row, and the quality controls
    'expect @radio:gen_stage_final'
    'expect @check:gen_low_memory'
    'expect @slider:gen_sparse'
    "shot $work\h3_card_before.jpg"
    'click @combo:gen_continue'
    'click @option:gen_continue_previous'
    'wait 400'
    'click @radio:gen_stage_final'
    'wait 400'
    'click @check:gen_low_memory'
    'wait 400'
    "shot $work\h3_card_after.jpg"
    'click @button:gen_continue_new'        # a third clip, made in one click, after the second and carrying on from it
    'wait 700'
    "shot $work\h3_card_third.jpg"
  )
  $failed = $run.Errors
  try {
    if (-not $failed) {
      $clips = @((Invoke-Attome $run --json gen status $proj | ConvertFrom-Json).result.clips)
      if ($clips.Count -ne 3) { $failed = "expected three generative clips (the third made by Continue with a new clip), found $($clips.Count)" }
      else {
        $third = Get-Object $run $clips[2].clip
        $wf3 = $third.media_ref.workflow
        $ref3 = $wf3.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq 'attome.clip_reference' } | Select-Object -First 1
        "third: record_in=$($third.timing.record_in) source_in=$($third.timing.source_in) reference clip=$($ref3.Value.settings.clip)"
        if ($third.timing.record_in -ne '10' -or $third.timing.source_in -ne '11/12') { $failed = "the third clip is not at the end of the second, skipping the overlap ($($third.timing.record_in), $($third.timing.source_in))" }
        elseif ($ref3.Value.settings.clip -ne $clips[1].clip) { $failed = 'the third clip does not carry on from the second' }
      }
      if (-not $failed) {
        $second = Get-Object $run $clips[1].clip
        $wf = $second.media_ref.workflow
        $gen = $wf.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq 'attome.generate_video' } | Select-Object -First 1
        $ref = $wf.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq 'attome.clip_reference' } | Select-Object -First 1
        $link = $wf.links.PSObject.Properties | Where-Object { $_.Value.to[1] -eq 'continue_from' -and $ref -and $_.Value.from[0] -eq $ref.Name } | Select-Object -First 1
        "second: source_in=$($second.timing.source_in) stage=$($gen.Value.settings.stage) low_memory=$($gen.Value.settings.low_memory) reference=$($ref.Name)"
        if ($second.timing.source_in -ne '11/12') { $failed = "the clip does not skip the 22 frames that repeat the clip before (source_in $($second.timing.source_in))" }
        elseif (-not $ref -or $ref.Value.settings.clip -ne 'previous') { $failed = 'there is no Clip Reference node on the clip before' }
        elseif (-not $link) { $failed = 'the reference is not linked to the node''s continue_from' }
        elseif (@($wf.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq 'attome.get_frame' }).Count) { $failed = 'continuing made it start on a frame as well' }
        elseif ($gen.Value.settings.stage -ne 'final') { $failed = 'the Final quality was not kept in the node''s settings' }
        elseif ($gen.Value.settings.low_memory -ne 'on') { $failed = 'Low memory was not kept in the node''s settings' }
        else {
          $first = Get-Object $run $clips[0].clip
          $g1 = $first.media_ref.workflow.nodes.PSObject.Properties | Where-Object { $_.Value.kind -eq 'attome.generate_video' } | Select-Object -First 1
          if ($first.timing.source_in -ne '0' -or $g1.Value.settings.stage -ne 'preview') { $failed = 'the first clip changed with the second' }
        }
      }
    }
  } finally { Stop-Daemon $run }
} finally { Remove-Item Env:\ATTOME_MOCK_ENGINE, Env:\ATTOME_MODELS_DIR -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a clip set to continue the one before, to Final and to Low memory, each held in its own workflow (captures in $work)" -ForegroundColor Green
exit 0
