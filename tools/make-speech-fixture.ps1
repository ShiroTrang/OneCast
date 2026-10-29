<#
.SYNOPSIS
    Regenerates Tests/data/speech48k.wav, the speech fixture used by the DSP tests.

.DESCRIPTION
    Synthesises three short English sentences with the Windows SAPI voice
    "Microsoft Zira Desktop" (falling back to "Microsoft David Desktop") and
    writes them as a 48 kHz, 16-bit, mono WAV.

    Each sentence is synthesised on its own and trimmed of the silence SAPI pads
    around it, then the three are joined with exact stretches of digital
    silence: 0.3 s lead-in, 0.8 s between sentences, 0.3 s tail. Those gaps are
    the noise-only stretches the noise-reduction tests measure once they add
    noise of their own, so their length must not depend on the voice's padding.

    The WAV is committed, so the tests never need SAPI. Run this only to change
    the fixture:  powershell -File tools\make-speech-fixture.ps1
#>
[CmdletBinding()]
param(
    [string]$OutFile = (Join-Path $PSScriptRoot '..\Tests\data\speech48k.wav')
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Speech

$sampleRate = 48000
$sentences = @(
    'Hello everyone, thanks for joining the stream today.',
    'I will walk you through the setup step by step.',
    'If anything sounds off, just tell me in the chat.'
)

$leadMs  = 300
$pauseMs = 800
$tailMs  = 300

# Samples at or below this magnitude (about -84 dBFS) count as padding.
$silenceThreshold = 2

function New-Silence([int]$milliseconds)
{
    return , (New-Object byte[] ([int]($sampleRate * $milliseconds / 1000) * 2))
}

function Remove-EdgeSilence([byte[]]$pcm)
{
    $count = [int]($pcm.Length / 2)

    $first = 0
    while ($first -lt $count -and
           [Math]::Abs([int][BitConverter]::ToInt16($pcm, 2 * $first)) -le $silenceThreshold) { ++$first }

    $last = $count - 1
    while ($last -gt $first -and
           [Math]::Abs([int][BitConverter]::ToInt16($pcm, 2 * $last)) -le $silenceThreshold) { --$last }

    $trimmed = New-Object byte[] (($last - $first + 1) * 2)
    [Array]::Copy($pcm, 2 * $first, $trimmed, 0, $trimmed.Length)
    return , $trimmed
}

$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
try
{
    $installed = @($synth.GetInstalledVoices() | ForEach-Object { $_.VoiceInfo.Name })
    $voice = @('Microsoft Zira Desktop', 'Microsoft David Desktop') |
                 Where-Object { $installed -contains $_ } | Select-Object -First 1

    if (-not $voice)
    {
        throw "Neither Zira nor David is installed. Installed voices: $($installed -join ', ')"
    }

    $synth.SelectVoice($voice)
    $synth.Rate   = 0
    $synth.Volume = 100

    $format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(
        $sampleRate,
        [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
        [System.Speech.AudioFormat.AudioChannel]::Mono)

    $pcm = New-Object System.IO.MemoryStream

    $silence = New-Silence $leadMs
    $pcm.Write($silence, 0, $silence.Length)

    for ($i = 0; $i -lt $sentences.Count; ++$i)
    {
        # SetOutputToAudioStream delivers raw PCM in the requested format.
        $raw = New-Object System.IO.MemoryStream
        $synth.SetOutputToAudioStream($raw, $format)
        $synth.Speak($sentences[$i])
        $synth.SetOutputToNull()

        $speech = Remove-EdgeSilence $raw.ToArray()
        $pcm.Write($speech, 0, $speech.Length)

        $silence = New-Silence $(if ($i -lt $sentences.Count - 1) { $pauseMs } else { $tailMs })
        $pcm.Write($silence, 0, $silence.Length)
    }

    $data = $pcm.ToArray()
    $target = [System.IO.Path]::GetFullPath($OutFile)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null

    $file = [System.IO.File]::Create($target)
    $writer = New-Object System.IO.BinaryWriter($file)
    try
    {
        $ascii = [System.Text.Encoding]::ASCII
        $writer.Write($ascii.GetBytes('RIFF'))
        $writer.Write([int](36 + $data.Length))
        $writer.Write($ascii.GetBytes('WAVE'))
        $writer.Write($ascii.GetBytes('fmt '))
        $writer.Write([int]16)                 # PCM format chunk size
        $writer.Write([int16]1)                # PCM
        $writer.Write([int16]1)                # mono
        $writer.Write([int]$sampleRate)
        $writer.Write([int]($sampleRate * 2))  # byte rate
        $writer.Write([int16]2)                # block align
        $writer.Write([int16]16)               # bits per sample
        $writer.Write($ascii.GetBytes('data'))
        $writer.Write([int]$data.Length)
        $writer.Write($data)
    }
    finally
    {
        $writer.Dispose()
    }

    Write-Host ("Wrote {0} using {1}: {2:N2} s" -f $target, $voice, ($data.Length / 2 / $sampleRate))
}
finally
{
    $synth.Dispose()
}
