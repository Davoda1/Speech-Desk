# Security and privacy

SpeechDesk 1.0.x is the supported release line. Report security problems using the repository's private vulnerability-reporting option when it is available. If it is unavailable, open an issue requesting a private reporting channel without posting sensitive details, exploit payloads or credentials.

## Data handling

- Speech-to-text runs locally. Microphone recordings are not sent to a hosted recognition service.
- TTS sends the requested text and synthesis options to the selected speech service. That service controls its own logging, retention and billing.
- API keys remain in memory for the application session. Settings and profile exports omit dedicated API keys and standard authentication headers, including ElevenLabs' `xi-api-key`.
- Custom headers and request-template/body fields may be saved. Never place credentials in those fields or in URLs.
- Text and generated audio are saved only through the explicit save actions. Temporary audio is removed on normal cleanup; abnormal termination may leave temporary files.
- The application does not include telemetry or automatically download recognition models.

HTTP redirects are not followed. Endpoint paths and returned audio URLs must remain on the configured origin; normal TLS certificate checks remain enabled. Use HTTPS for hosted services. Review imported profiles before making a request.

Optional-model imports validate the pinned model identity, size and checksum and reject unsafe archive paths. Import only model packs intended for SpeechDesk. Release checksums are published in one `SHA256SUM.txt` file.

Use synthetic text and audio in bug reports. Remove API keys, authorization headers, full user paths, private hostnames and personal content from diagnostics before posting them.
