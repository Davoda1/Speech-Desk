# SpeechDesk 1.1.1

SpeechDesk 1.1.1 is a patch release for dictation layout and interface documentation.

## Changes in 1.1.1

- Size speech and dictation dropdowns for complete labels, including the dropdown arrow and padding. Long selected values and popup entries wrap when needed.
- Center dictation selectors and action buttons in separate groups, keeping each wrapped row centered at narrower window widths.
- Measure the Live text button using its rendered font and preserve padding at different display scales.
- Include eight fresh screenshots covering the editor, dictation, server settings and About in dark and light themes. The screenshots are available separately and within the offline documentation.

## Features introduced in 1.1.0

SpeechDesk 1.1.0 introduced optional encrypted credentials, OpenAI Realtime support and refinements to the native Qt6 interface:

- Save an API key separately for each server profile when requested. Argon2id and XChaCha20-Poly1305 protect the saved credential; the unlock passphrase is never stored.
- Unlock saved keys when needed, lock them again during a session, or replace an inaccessible credential without changing another profile's key. Keys remain session-only unless saving is enabled.
- Use OpenAI Realtime for its replacement speech model. The older OpenAI speech models remain available before their announced 6 January 2027 shutdown and are hidden and rejected from that date onward. Compatible self-hosted speech APIs are unaffected.
- See microphone input on a centered meter that expands toward both ends as the input level increases.
- Toggle Live text with a button matching the surrounding controls; its pressed state indicates that provisional dictation text is enabled.
- Follow clearer reading-part progress while retaining playback, text highlighting and full-reading WAV export.

## Packaging and compatibility

Windows and Linux runtime packages include Qt and the four default local recognition models. Windows ZIP and Linux tar.gz packages start their top-level executable directly; the Debian package installs the real binary at `/usr/bin/speechdesk`. Whisper Large v3 Turbo and Large v3 remain separate, complete ZIP add-ons.

TTS compatibility depends on the service, model, voice and response format. Realtime speech is buffered per reading part and its returned transcript is checked against the requested text before playback. A mismatch or missing transcript causes an error; punctuation or number-format differences can also fail this conservative check. Transcript matching does not establish pronunciation accuracy. All speech recognition remains local.

See [Verification](Verification.md) for the recorded validation scope and [Server profiles](Server-Profiles.md) for configuration. Hosted providers apply their own account, retention and billing terms. Physical audio devices and private speech servers require checks on the target setup.
