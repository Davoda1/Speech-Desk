SpeechDesk 2.0.0 is a native desktop application for text-to-speech playback and local dictation on Windows and Linux.

This release adds bundled Pocket TTS and Kokoro CPU speech with 34 English voice presets through the existing provider controls. Offline speech needs no server, account, GPU, external Python installation or model download. Existing profiles and local dictation remain available.

## Features

- Write, paste, open text files, or dictate into one editor.
- Choose built-in Pocket TTS / Kokoro, OpenAI, ElevenLabs, OpenRouter, or a compatible local or remote speech server.
- Play, pause, resume, stop, and follow the spoken passage with highlighting.
- Export the complete reading as a WAV file.
- Dictate locally with bundled Whistle and Whisper Tiny, Base, and Small models.
- Choose system, light, or dark appearance. Provider keys remain session-only by default, with optional passphrase-protected encrypted storage. The application has no telemetry.

## Downloads

| Package | Use |
| --- | --- |
| `SpeechDesk-2.0.0-Windows-x64.zip` | Windows 10/11 x64; extract and open the root `SpeechDesk.exe` |
| `SpeechDesk-2.0.0-Linux-amd64.tar.gz` | Linux amd64 portable package; run the top-level `speechdesk` binary |
| `SpeechDesk-2.0.0-Linux-amd64.deb` | Debian-family installation with the real binary in `/usr/bin` |
| `SpeechDesk-2.0.0-Source.zip` / `.tar.gz` | Application source and pinned dependency reconstruction tools |
| `SpeechDesk-2.0.0-Whisper-Large-v3-Turbo-Expansion.zip` | Optional local dictation model; import the complete ZIP in Settings |
| `SpeechDesk-2.0.0-Whisper-Large-v3-Expansion.zip` | Optional larger local dictation model; import the complete ZIP in Settings |

Runtime packages include Qt, both offline speech models and their runtimes, and the four default recognition models. The larger models are separate add-ons. Download the HTML guide, preview, release notes, and fresh light/dark screenshots for setup and feature details. Verify downloaded assets against the single `SHA256SUM.txt` manifest.

## Compatibility and verification

The release workflow builds Windows and Linux packages, runs focused client tests, and validates package layout and checksums. Built-in model generation is checked separately from deterministic hosted-service fixtures. Physical microphones/speakers and live paid-provider accounts require checks on the target setup.

Speech sections are buffered before playback. Built-in voices use passage highlighting; word highlighting requires timings supplied by a compatible service.

Copyright © 2026 davoda1. The application is MIT-licensed. Bundled third-party components retain their own licenses and notices.
