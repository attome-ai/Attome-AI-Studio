param([string]$Out)
# Speaks a text with a Windows voice into a 16 kHz mono WAV and records when each word starts (SpeakProgress: the voice's own word positions).
Add-Type -AssemblyName System.Speech
$texts = @(
  "The quick brown fox jumps over the lazy dog, and then it runs into the forest to find something to eat before the night comes.",
  "Video editing is the art of choosing what to keep. Every cut changes the rhythm, and a good editor listens as much as they watch.",
  "Tomorrow we will travel to the mountains. The weather report says it will be cold, so bring a warm jacket, gloves, and a hat.",
  "Numbers can be tricky for speech recognition: twenty three people arrived at nine fifteen, and four hundred more came later that evening."
)
$voices = @('Microsoft David Desktop', 'Microsoft Zira Desktop')
$all = @()
$i = 0
foreach ($t in $texts) {
  foreach ($v in $voices) {
    $s = New-Object System.Speech.Synthesis.SpeechSynthesizer
    $s.SelectVoice($v)
    $s.Rate = 0
    $fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
    $wav = Join-Path $Out ("tts_{0}.wav" -f $i)
    $s.SetOutputToWaveFile($wav, $fmt)
    $words = New-Object System.Collections.ArrayList
    Register-ObjectEvent -InputObject $s -EventName SpeakProgress -SourceIdentifier "sp$i" -Action { [void]$Event.MessageData.Add(@{ t = $EventArgs.Text; s = $EventArgs.AudioPosition.TotalSeconds }) } -MessageData $words | Out-Null
    $s.Speak($t)
    Start-Sleep -Milliseconds 200
    Unregister-Event -SourceIdentifier "sp$i"
    $s.Dispose()
    $all += @{ file = (Split-Path $wav -Leaf); voice = $v; text = $t; words = @($words) }
    $i++
  }
}
$all | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $Out 'truth.json') -Encoding utf8
"made $i recordings"
