# UI test: what a clip offers (UX review 2: B2, B7, C2, D2).
#  - a wide video in a tall project: Fill covers the canvas, Fit shows it all again (Transform card)
#  - a clip selected away from the playhead: the Monitor says so and "Go to it" moves the playhead there
#  - a corner handle in the Monitor resizes the selected clip, a bar on a side crops it
#  - with the picture of a video selected, the Audio card of its linked sound is shown and works
#  - a double click on a text in the Monitor edits its words there
#  - the clip's menu has Go to its start, Fade, and for a sound clip Mute and Unlink
# Virtual input only (uitest.psm1). Needs a build. Exit code 0 = pass.
#   .\tools\uitest\ux_clip_tools.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Tools.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\wide.mp4" "--seconds 4 --height 360"     # 640 x 360
  & "$bin\attome.exe" new $proj --rate 30 --canvas 1080x1920 | Out-Null
  [IO.File]::WriteAllText("$work\call.json", (@{ project = $proj; ops = @(@{ op = 'add_clip'; path = "$work\wide.mp4"; at = '0s' },
        @{ op = 'add_text'; text = 'Later'; name = 'Later'; at = '2s'; duration = '2s' }) } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
  $r = & "$bin\attome.exe" --json call timeline.edit "$work\call.json" | ConvertFrom-Json
  if (-not $r.ok) { throw "setup: $($r.error.message)" }
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

function Picture($run) { $t = @(Get-Tracks $run) | Where-Object { $_.name -eq 'V1' }; Get-Object $run @($t.clip_list)[0].id }
function Sound($run) { $t = @(Get-Tracks $run) | Where-Object { $_.kind -eq 'audio' } | Select-Object -First 1; Get-Object $run @($t.clip_list)[0].id }

$failed = $null
# The picture clip is selected (the first clip of the film); the playhead is in it.
$run = Invoke-EditorScript -Project $proj -SelectFirstClip -TimeoutSeconds 240 -Script @(
  'wait 1200'
  'expect @button:scale_fill', 'expect @button:scale_fit'
  "shot $work\tools_letterbox.jpg"
  'click @button:scale_fill', 'wait 800'
  "shot $work\tools_fill.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $s = (Picture $run).transform.scale
    "after Fill: scale $($s -join ' x ')"
    # 640 x 360 in 1080 x 1920: fitted by width (1.6875), filled by height (5.333): 3.16 times the fitted size
    if ([math]::Abs($s[0] - 3.16) -gt 0.02 -or [math]::Abs($s[1] - 3.16) -gt 0.02) { $failed = "Fill gave a scale of $($s -join ' x '), not 3.16" }
  }
  if (-not $failed) {
    $fit = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('click @button:scale_fit', 'wait 800')
    $failed = $fit.Errors
    if (-not $failed) { $s = (Picture $run).transform.scale; if ([math]::Abs($s[0] - 1.0) -gt 0.005) { $failed = "Fit gave a scale of $($s[0]), not 1" } }
  }
  if (-not $failed) { # the picture is selected: the Audio card of its linked sound is there too, and changes the sound clip
    $au = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('expect @slider:gain', 'click @number:gain', 'wait 300', 'type -6', 'key Enter', 'wait 800', "shot $work\tools_audio_of_picture.jpg")
    $failed = $au.Errors
    if (-not $failed) {
      $g = (Sound $run).audio.gain_db
      "gain of the linked sound, set with the picture selected: $g dB"
      if ($g -ne -6) { $failed = "the gain of the linked sound is '$g', not -6" }
    }
  }
  if (-not $failed) { # a corner handle in the Monitor makes the clip larger about its middle, keeping its shape; one Undo takes it back
    $h = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('expect @handle:2', "shot $work\tools_handles.jpg", 'drag @handle:2 40 22', 'wait 800', "shot $work\tools_scaled.jpg")
    $failed = $h.Errors
    if (-not $failed) {
      $s = (Picture $run).transform.scale
      "after dragging a corner out: scale $($s -join ' x ')"
      if ($s[0] -lt 1.15 -or [math]::Abs($s[0] - $s[1]) -gt 0.002) { $failed = "the corner handle left a scale of $($s -join ' x ')" }
    }
    if (-not $failed) {
      $u = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl', 'wait 700')
      $failed = $u.Errors
      if (-not $failed -and [math]::Abs((Picture $run).transform.scale[0] - 1.0) -gt 0.005) { $failed = 'Undo did not take the handle drag back' }
    }
  }
  if (-not $failed) { # the bar on the left side crops it: dragged across to the right bar, the most it may cut (95 %); Undo gives it back
    $probe = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('expect @cropbar:left', 'expect @cropbar:right')
    $failed = $probe.Errors
    if (-not $failed) {
      # Dragged all the way to the right bar: the left side is cut to the most it may be (95 % less what the right side has).
      $cr = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('drag @cropbar:left @cropbar:right', 'wait 50', "shot $work\tools_cropping.jpg", 'wait 800')
      $failed = $cr.Errors
    }
    if (-not $failed) {
      $crop = (Picture $run).transform.crop
      "crop after dragging the left bar across: $($crop | ConvertTo-Json -Compress)"
      if (-not $crop -or $crop.left -lt 0.85 -or $crop.left -gt 0.951 -or $crop.right -ne 0 -or $crop.top -ne 0) { $failed = "the left bar left a crop of $($crop | ConvertTo-Json -Compress)" }
    }
    if (-not $failed) {
      $u = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl', 'wait 700')
      $failed = $u.Errors
      $crop = (Picture $run).transform.crop
      if (-not $failed -and $crop -and $crop.left -gt 0) { $failed = 'Undo did not take the crop back' }
    }
    if (-not $failed) { # the right bar dragged in by a quarter of the picture's width in the Monitor (the picture fills the 9:16 frame's width): a quarter is cut
      $q = Invoke-EditorScript -Project $proj -SelectFirstClip -Endpoint $run.Endpoint -Script @('drag @cropbar:right -54 0', 'wait 800')
      $failed = $q.Errors
      if (-not $failed) {
        $crop = (Picture $run).transform.crop
        "crop after dragging the right bar 54 px in (the picture is 216 px wide there): $($crop | ConvertTo-Json -Compress)"
        if ([math]::Abs($crop.right - 0.25) -gt 0.04 -or $crop.left -ne 0) { $failed = "the right bar left a crop of $($crop | ConvertTo-Json -Compress)" }
      }
      if (-not $failed) { $u = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('key Z ctrl', 'wait 700'); $failed = $u.Errors }
    }
  }
  if (-not $failed) { # a clip that starts at 2 s is selected while the playhead is before it: the Monitor offers the way there
    $go = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'key Home', 'wait 300'
      'click @clip:Later', 'wait 500'
      'expect @button:goto_clip'
      "shot $work\tools_not_at_playhead.jpg"
      'click @button:goto_clip', 'wait 600'
      'absent @button:goto_clip'
      "shot $work\tools_at_clip.jpg"
      'key Home', 'wait 300', 'expect @button:goto_clip'
      'dblclick @clip:Later', 'wait 600', 'absent @button:goto_clip'      # a double click on a clip goes to it too
    )
    $failed = $go.Errors
  }
  if (-not $failed) { # a double click on a text in the Monitor: its words are typed there, and kept by a click elsewhere
    $txt = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'click @clip:Later', 'wait 300', 'dblclick @clip:Later', 'wait 500'      # the playhead goes to it
      'dblclick @monitor', 'wait 500', 'expect @field:monitor_text'
      'key A ctrl', 'type Sooner', 'wait 300'
      "shot $work\tools_text_on_picture.jpg"
      'click @track:V1', 'wait 800'
      'absent @field:monitor_text')
    $failed = $txt.Errors
    if (-not $failed) {
      $t = @(Get-Tracks $run) | Where-Object { $_.name -eq 'Titles' }
      $said = (Get-Object $run @($t.clip_list)[0].id).content.text
      "the title after typing on the picture: '$said'"
      if ($said -ne 'Sooner') { $failed = "the text is '$said', not 'Sooner'" }
    }
  }
  if (-not $failed) { # the menu of the text clip: Fade opens the Fade card
    $menu = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'rclick @clip:Later', 'wait 400'
      'expect @menuitem:Go_to_its_start', 'expect @menuitem:Fade_in_and_out...'
      "shot $work\tools_menu.jpg"
      'click @menuitem:Fade_in_and_out...', 'wait 500'
      'expect @slider:fadein'
    )
    $failed = $menu.Errors
  }
  if (-not $failed) { # the menu of the sound clip (the mark of the name is the sound's: it is drawn last): Mute, then Unlink
    $snd = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @(
      'rclick @clip:wide', 'wait 400'
      'expect @menuitem:Mute', 'expect @menuitem:Unlink_from_its_picture'
      'click @menuitem:Mute', 'wait 700'
    )
    $failed = $snd.Errors
    if (-not $failed) {
      $v = (Sound $run).volume
      "sound volume after Mute: $v"
      if ($v -ne 0) { $failed = "Mute from the menu left a volume of $v" }
    }
  }
  if (-not $failed) {
    $un = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('rclick @clip:wide', 'wait 400', 'expect @menuitem:Unmute', 'click @menuitem:Unlink_from_its_picture', 'wait 800')
    $failed = $un.Errors
    if (-not $failed) {
      $names = (Sound $run).PSObject.Properties.Name
      if ($names -contains 'link_group') { $failed = 'Unlink from the menu left the sound linked' }
    }
  }
  if (-not $failed) {
    $valid = Invoke-Attome $run --json validate $proj | ConvertFrom-Json
    if (-not $valid.result.ok) { $failed = "the project is not valid: $($valid.result | ConvertTo-Json -Compress -Depth 4)" }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: Fill and Fit; the Monitor leads to a clip selected away from the playhead; the clip menu has Go to, Fade, Mute and Unlink (captures in $work)" -ForegroundColor Green
exit 0
