# Security and privacy

SpeechDesk 1.1.x is the supported release line. Report security problems using the repository's private vulnerability-reporting option when it is available. If it is unavailable, open an issue requesting a private reporting channel without posting sensitive details, exploit payloads or credentials.

## Data handling

- Speech-to-text runs locally. Microphone recordings are not sent to a hosted recognition service.
- TTS sends the requested text and synthesis options to the selected speech service. That service controls its own logging, retention and billing.
- API keys remain session-only by default. Optional encrypted storage keeps a separate credential for each profile. Settings and profile exports omit plaintext API keys and standard authentication headers, including ElevenLabs' `xi-api-key`.
- Custom headers and request-template/body fields may be saved. Never place credentials in those fields or in URLs.
- Text and generated audio are saved only through the explicit save actions. Temporary audio is removed on normal cleanup; abnormal termination may leave temporary files.
- The application does not include telemetry or automatically download recognition models.

HTTP redirects are not followed. Endpoint paths and returned audio URLs must remain on the configured origin; normal TLS certificate checks remain enabled. Use HTTPS for hosted services. Review imported profiles before making a request.

## Encrypted credentials

Saving a key is optional and does not depend on an operating-system keychain. The credential format uses libsodium's Argon2id moderate preset (256 MiB memory) to derive a 256-bit key, then XChaCha20-Poly1305 authenticated encryption. Every encryption uses a fresh random salt and nonce. The encrypted record is bound to its profile identity, provider and normalized service origin. A changed origin requires credentials for the new destination.

The passphrase is never written to settings, a credential file, an export or a log. Use a long, unique passphrase: protection is limited by passphrase strength. The format and derivation parameters are checked before expensive processing. Unsupported, corrupt or incorrectly unlocked records are preserved rather than overwritten. New records replace existing records atomically after successful encryption.

Unlocking makes the API key available in process memory for that session. **Lock now** clears the application's active copy; memory cleanup is best effort and cannot protect against malware running as the user, memory dumps or operating-system swap. Encryption protects a saved credential while locked; it is not a claim that the whole application is quantum-proof or that every memory copy can be securely erased.

If a passphrase is forgotten, replace only the affected profile's credential by entering a new or re-entered API key and choosing a new passphrase. No recovery service or stored passphrase exists. Turning encrypted saving off or deleting the profile removes its saved credential. Removing or replacing a file does not guarantee erasure from storage-device history, filesystem snapshots or backups.

Optional-model imports validate the pinned model identity, size and checksum and reject unsafe archive paths. Import only model packs intended for SpeechDesk. Release checksums are published in one `SHA256SUM.txt` file.

Use synthetic text and audio in bug reports. Remove API keys, authorization headers, full user paths, private hostnames and personal content from diagnostics before posting them.
