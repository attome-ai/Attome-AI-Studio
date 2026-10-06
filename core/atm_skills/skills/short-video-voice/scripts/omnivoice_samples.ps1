$text = "Hour three: school pranks. The teacher's pencil is haunted. Hour six: skip every line at the theme park!"
$dir = "C:\Users\Computia.me\Documents\Attome\voice_samples2"; New-Item -ItemType Directory -Force $dir | Out-Null
$variants = @(
  @{ n = 'B1_64steps';      i = 'male, young adult, moderate pitch, american accent'; s = 1.0; st = 64; cfg = 2.0; dn = $false; ts = 1.0 },
  @{ n = 'B2_cfg3_slow';    i = 'male, young adult, moderate pitch, american accent'; s = 0.95; st = 48; cfg = 3.5; dn = $false; ts = 1.0 },
  @{ n = 'B3_denoise';      i = 'male, young adult, moderate pitch, american accent'; s = 1.0; st = 48; cfg = 2.0; dn = $true; ts = 1.0 },
  @{ n = 'D1_64steps';      i = 'male, young adult, low pitch, british accent'; s = 1.0; st = 64; cfg = 2.0; dn = $false; ts = 1.0 },
  @{ n = 'D2_cfg3_slow';    i = 'male, young adult, low pitch, british accent'; s = 0.95; st = 48; cfg = 3.5; dn = $false; ts = 1.0 },
  @{ n = 'D3_denoise';      i = 'male, young adult, low pitch, british accent'; s = 1.0; st = 48; cfg = 2.0; dn = $true; ts = 1.0 })
foreach ($v in $variants) {
  $g = @{
    'l' = @{ class_type = 'OmniVoiceLoader'; inputs = @{ 'OmniVoice Model' = 'model.safetensors'; 'Audio Tokenizer Model' = 'audio_tokenizer.safetensors'; 'Keep model in VRAM' = $true } }
    't' = @{ class_type = 'OmniVoiceTTS'; inputs = @{ model = @('l', 0); text = $text; language = 'English'; instruct = $v.i; speed = $v.s; duration = 0.0; num_step = $v.st; cfg = $v.cfg; seed = 11; t_shift = $v.ts; denoise = $v.dn; preprocess_prompt = $true; postprocess_output = $true } }
    's' = @{ class_type = 'SaveAudio'; inputs = @{ audio = @('t', 0); filename_prefix = "sample_$($v.n)" } } }
  $r = Invoke-RestMethod -Method Post -Uri http://127.0.0.1:8188/prompt -ContentType 'application/json' -Body (@{ prompt = $g; client_id = 'samples' } | ConvertTo-Json -Depth 10)
  $id = $r.prompt_id
  for ($k = 0; $k -lt 240; $k++) {
    Start-Sleep -Milliseconds 500
    $h = Invoke-RestMethod "http://127.0.0.1:8188/history/$id"
    if ($h.$id) {
      $a = $h.$id.outputs.s.audio[0]
      Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:8188/view?filename=$($a.filename)&subfolder=$($a.subfolder)&type=$($a.type)" -OutFile "$dir\$($v.n).flac"
      "$($v.n) $($h.$id.status.status_str)"; break
    }
  }
}

