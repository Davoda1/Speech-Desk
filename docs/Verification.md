# Verification scope

## SpeechDesk 2.0.0

This release adds bundled Pocket TTS and Kokoro to the existing speech pipeline. The established CTest groups cover text segmentation, HTTP/WebSocket providers, cancellation, model-pack import, encrypted credentials and UI state. Focused additions exercise offline model/voice catalogs, provider switching and failure/cancellation behavior using the same test framework.

Real CPU generation is checked separately using the pinned runtime assets. The worker and model pins come from the current ThreadShelf Reader implementation. SpeechDesk receives completed PCM16 WAV sections; it does not concatenate raw streaming chunks. Audio integrity checks cover generated WAV format, clipping/discontinuities and preservation through Qt decoding/export. The native Linux Release build used Qt 6.8.2 and pinned libsodium 1.0.22. All six CTest groups passed, including the added offline provider and transport cases. Both engines also passed generation through the actual TtsClient adapter; cancel/retry completed without stale output or leftover session folders.

## Reproduce the focused tests

```sh
python3 vendor/tools/build_sodium.py --output build/deps/sodium --cc gcc --jobs 3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSPEECHDESK_BUILD_TESTS=ON
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure --timeout 90
```

The release workflow runs the existing native Windows/Linux builds and tests, verifies the pinned speech assets, generates real speech with both engines, checks package layout and produces one checksum manifest. Software workflow completion is recorded by GitHub after dispatch; a locally passed Linux build is not a native Windows runtime result.

Hosted adapters use deterministic local fixtures; no live paid-provider request is claimed. Physical microphone/speaker behavior and subjective listening quality require the target audio setup. Word highlighting uses genuine provider timing data; built-in speech uses passage highlighting.

## Pre-upload audio integrity checks

The exact current Reader worker and model pins were used. Four multi-sentence samples exercised numbers, sibilants and pauses through the actual TtsClient adapter, then through the existing AudioMerger WAV export.

| Model / voice | Audio duration | Clipped samples | Worker WAV preserved | Export sample comparison |
| --- | ---: | ---: | --- | --- |
| pocket-alba | 16.680 s | 0 | Byte-for-byte | Exact PCM match |
| pocket-marius | 17.560 s | 0 | Byte-for-byte | Exact PCM match |
| kokoro-heart | 17.941 s | 0 | Byte-for-byte | Exact PCM match |
| kokoro-george | 19.115 s | 0 | Byte-for-byte | Exact PCM match |

All four exports had zero sample discontinuity at the tested section join. Exported PCM matched an independent FFmpeg conversion sample-for-sample. Qt omits 32 resampler tail samples per section (0.667 ms at 48 kHz); their magnitude was at most one PCM16 unit. Original playback WAV files remain intact. These objective checks found no waveform corruption in the tested paths; no subjective listening assessment is claimed.

Copyright © 2026 davoda1.
