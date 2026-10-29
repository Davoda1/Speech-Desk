# SpeechDesk 1.0.0

First release of SpeechDesk, a native C++17 / Qt6 application for speech playback and local dictation.

## Included

- One editor for typed text, text files and locally recognized speech.
- Hosted TTS profiles for OpenAI, ElevenLabs and OpenRouter, plus local/remote self-hosted speech services and configurable HTTP APIs.
- Bundled Whistle and multilingual Whisper Tiny, Base and Small recognition models.
- Separate complete ZIP add-ons for Whisper Large v3 Turbo and Large v3.
- Playback controls, text highlighting, selected-passage reading and full-reading WAV export.
- System/light/dark themes, embedded application icons and a resizable interface.
- Session-only provider credentials, local speech recognition and no application telemetry.

Windows and Linux runtime packages include Qt and recognition assets. Windows ZIP and Linux tar.gz packages start their top-level executable directly; the Debian package installs the real binary at `/usr/bin/speechdesk`.

## Compatibility notes

TTS compatibility depends on the service, model, voice and supported format. Sections are buffered before playback; SSE deltas do not provide incremental low-latency playback. Word highlighting requires supported timing data. Voice cloning, reference uploads, WebSocket-only services and asynchronous job APIs are outside this release.

See [Verification](Verification.md) for the recorded validation scope and [Server profiles](Server-Profiles.md) for service configuration. Hosted providers apply their own account, retention and billing terms.
