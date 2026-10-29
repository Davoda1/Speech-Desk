SpeechDesk 1.0.0 is a native desktop application for text-to-speech playback and local dictation on Windows and Linux.

## Features

- Write, paste, open text files, or dictate into one editor.
- Use OpenAI, ElevenLabs, OpenRouter, or a compatible local or remote speech server.
- Play, pause, resume, stop, and follow the spoken passage with highlighting.
- Export the complete reading as a WAV file.
- Dictate locally with bundled Whistle and Whisper Tiny, Base, and Small models.
- Choose system, light, or dark appearance. Provider keys remain session-only; the application has no telemetry.

## Downloads

| Package | Use |
| --- | --- |
| `SpeechDesk-1.0.0-Windows-x64.zip` | Windows 10/11 x64; extract and open the root `SpeechDesk.exe` |
| `SpeechDesk-1.0.0-Linux-amd64.tar.gz` | Linux amd64 portable package; run the top-level `speechdesk` binary |
| `SpeechDesk-1.0.0-Linux-amd64.deb` | Debian-family installation with the real binary in `/usr/bin` |
| `SpeechDesk-1.0.0-Source.zip` / `.tar.gz` | Application source and pinned dependency reconstruction tools |
| `SpeechDesk-1.0.0-Whisper-Large-v3-Turbo-Expansion.zip` | Optional local dictation model; import the complete ZIP in Settings |
| `SpeechDesk-1.0.0-Whisper-Large-v3-Expansion.zip` | Optional larger local dictation model; import the complete ZIP in Settings |

Runtime packages include Qt and the four default recognition models. The larger models are separate add-ons. Download the HTML guide, preview, release notes, and screenshots for setup and feature details. Verify downloaded assets against the single `SHA256SUM.txt` manifest.

## Compatibility and verification

The release workflow builds Windows and Linux packages, runs focused client tests, and validates package layout and checksums. One live OpenRouter request was verified through the production TTS client. Direct OpenAI and ElevenLabs account integration and physical microphones/speakers require checks on the target setup.

Speech sections are buffered before playback. Word highlighting requires timing data from the service; otherwise the active passage is highlighted. Model, voice, format, availability, retention, and billing rules depend on the selected provider.

OpenAI has announced retirement of the current Speech API models on January 6, 2027. This version uses that HTTP API; its replacement Realtime protocol requires a future adapter. See the [provider guide](https://github.com/Davoda1/Speech-Desk/blob/master/docs/Server-Profiles.md).

The application is MIT-licensed. Bundled third-party components retain their own licenses and notices.
