# SpeechDesk 2.0.0

SpeechDesk now includes offline CPU speech alongside hosted and self-hosted providers.

- Add Built-in / Offline to the existing provider controls, with Pocket TTS and Kokoro models and their 34 English preset voices.
- Bundle the same pinned, full-precision speech assets used by ThreadShelf Reader, including the Python runtime for Kokoro. No runtime downloads, external Python installation, account or GPU are required.
- Reuse section buffering, playback, passage highlighting, cancellation and complete-reading WAV export.
- Preserve saved provider profiles, credentials, editor behavior and local dictation on upgrade.
- Refresh all light/dark screenshots and update online/offline documentation and release metadata to 2.0.0.

## Packaging

Windows ZIP and Linux tar.gz packages start their top-level executable directly; the Debian package installs the real binary at `/usr/bin/speechdesk`. Runtime packages include both TTS models, their platform runtimes, Qt, and the four default recognition models. Whisper Large v3 Turbo and Large v3 remain complete optional ZIP add-ons. Model/runtime hashes and required third-party license notices are included. Downloads are covered by one SHA256SUM.txt manifest.

## Verification

The established Windows/Linux build and focused test workflow remains in use, with additional coverage for offline provider behavior. Release packaging checks bundled speech assets and exercises both engines. See [Verification](Verification.md) for the actual validation scope. Hosted provider availability and physical audio devices remain dependent on the target setup.

Copyright © 2026 davoda1. SpeechDesk is MIT-licensed; third-party components retain their own licenses.
