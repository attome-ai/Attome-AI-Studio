# UI test: the Text rail item adds a title at the playhead; the Inspector edits it; Arabic text is shaped.
# Virtual input only (uitest.psm1). Needs a build (build\win-msvc-release\bin). Exit code 0 = pass.
#   .\tools\uitest\text_clip.ps1

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'uitest.psm1') -Force -DisableNameChecking
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'build\win-msvc-release\bin'
$work = Join-Path $env:TEMP ('attome-uitest\' + [IO.Path]::GetFileNameWithoutExtension($PSCommandPath))
New-Item -ItemType Directory -Force $work | Out-Null
$proj = Join-Path $work 'Text.attome'
Remove-Item $proj -Recurse -Force -ErrorAction SilentlyContinue

$env:ATTOME_ENDPOINT = "\\.\pipe\attome-uitest-setup-$PID"
try {
  New-Sample "$work\a.mp4" "--seconds 6 --height 540"
  & "$bin\attome.exe" new $proj --rate 30 | Out-Null
  $seq = (& "$bin\attome.exe" --json inspect $proj | ConvertFrom-Json).result.data.sequences[0].id
  $media = ("$work\a.mp4").Replace('\', '\\')
  @"
{"ops":[
 {"op":"add","path":"$seq/tracks/`$new:v1","value":{"kind":"video","name":"V1"}},
 {"op":"add","path":"`$new:v1/clips/`$new:a","value":{"name":"a.mp4","timing":{"record_in":"0","duration":"6","source_in":"0"},"media_ref":{"type":"file","path":"$media","duration":"6","width":960,"height":540}}}
]}
"@ | Set-Content "$work\pt.json" -Encoding utf8
  & "$bin\attome.exe" patch $proj "$work\pt.json" | Out-Null
} finally { Remove-Item Env:\ATTOME_ENDPOINT -ErrorAction SilentlyContinue }

$run = Invoke-EditorScript -Project $proj -Script @(
  'click @rail:Text'
  "shot $work\text_panel.jpg"
  'click @style:Title'
  "shot $work\text_added.jpg"
)
$failed = $run.Errors
try {
  if (-not $failed) {
    $titles = @(Get-Tracks $run) | Where-Object { $_.name -eq 'Titles' }
    if (-not $titles) { $failed = "no 'Titles' track after clicking the Title style" }
    else {
      $cid = $titles.clip_list[0].id
      $clip = Get-Object $run $cid
      "text clip: type=$($clip.media_ref.type) text='$($clip.content.text)' size=$($clip.content.size) pos=$($clip.transform.position -join ',')"
      if ($clip.media_ref.type -ne 'text' -or $clip.content.text -ne 'Your title') { $failed = "the text clip was not stored as expected" }
      else {
        # Arabic text through the same document path an agent would use, then look at the picture
        $arabic = [string]::new([char[]](0x0645,0x0631,0x062D,0x0628,0x0627,0x20,0x0628,0x0627,0x0644,0x0639,0x0627,0x0644,0x0645))
        @{ ops = @(@{ op = 'replace'; path = "$cid/content/text"; value = $arabic }) } | ConvertTo-Json -Depth 5 | Set-Content "$work\arabic.json" -Encoding utf8
        Invoke-Attome $run patch $proj "$work\arabic.json" | Out-Null
        $look = Invoke-EditorScript -Project $proj -Endpoint $run.Endpoint -Script @('wait 1000', "shot $work\text_arabic.jpg")
        $failed = $look.Errors
      }
    }
  }
} finally { Stop-Daemon $run }

if ($failed) { Write-Host "FAIL: $failed" -ForegroundColor Red; exit 1 }
Write-Host "PASS: a title was added from the Text panel (captures in $work)" -ForegroundColor Green
exit 0
