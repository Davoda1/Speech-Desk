SpeechDesk 1.1.1 is a native desktop application for text-to-speech playback and local dictation on Windows and Linux.

This patch keeps speech and dictation selector labels readable, corrects Live text button sizing, and centers dictation selectors and actions in separate groups. Eight fresh screenshots cover the editor, dictation, server settings and About in both themes.

## Features

- Write, paste, open text files, or dictate into one editor.
- Use OpenAI, ElevenLabs, OpenRouter, or a compatible local or remote speech server.
- Play, pause, resume, stop, and follow the spoken passage with highlighting.
- Export the complete reading as a WAV file.
- Dictate locally with bundled Whistle and Whisper Tiny, Base, and Small models.
- Choose system, light, or dark appearance. Provider keys remain session-only by default, with optional passphrase-protected encrypted storage. The application has no telemetry.

## Downloads

| Package | Use |
| --- | --- |
| `SpeechDesk-1.1.1-Windows-x64.zip` | Windows 10/11 x64; extract and open the root `SpeechDesk.exe` |
| `SpeechDesk-1.1.1-Linux-amd64.tar.gz` | Linux amd64 portable package; run the top-level `speechdesk` binary |
| `SpeechDesk-1.1.1-Linux-amd64.deb` | Debian-family installation with the real binary in `/usr/bin` |
| `SpeechDesk-1.1.1-Source.zip` / `.tar.gz` | Application source and pinned dependency reconstruction tools |
| `SpeechDesk-1.1.1-Whisper-Large-v3-Turbo-Expansion.zip` | Optional local dictation model; import the complete ZIP in Settings |
| `SpeechDesk-1.1.1-Whisper-Large-v3-Expansion.zip` | Optional larger local dictation model; import the complete ZIP in Settings |

Runtime packages include Qt and the four default recognition models. The larger models are separate add-ons. Download the HTML guide, preview, release notes, and eight screenshots for setup and feature details. Verify downloaded assets against the single `SHA256SUM.txt` manifest.

## Compatibility and verification

The release workflow builds Windows and Linux packages, runs focused client tests, and validates package layout and checksums. A live OpenRouter request was verified for version 1.0.0; it is not a live validation of the OpenAI Realtime adapter introduced in 1.1.0. Direct OpenAI and ElevenLabs account integration and physical microphones/speakers require checks on the target setup.

Speech sections are buffered before playback. Word highlighting requires timing data from the service; otherwise the active passage is highlighted. Model, voice, format, availability, retention, and billing rules depend on the selected provider.

OpenAI has announced retirement of the older Speech API models on January 6, 2027. SpeechDesk supports the replacement Realtime protocol introduced in 1.1.0 and hides and rejects retired OpenAI models from that date. Realtime audio is buffered and checked against its returned transcript before playback; mismatches are rejected. See the [provider guide](https://github.com/Davoda1/Speech-Desk/blob/master/docs/Server-Profiles.md).

The application is MIT-licensed. Bundled third-party components retain their own licenses and notices.
