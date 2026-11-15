# SpeechDesk bundled CPU speech

Copyright (c) 2026 davoda1. Application adapters and packaging are MIT licensed.
Third-party models, runtimes, voices and libraries retain their own licenses.

This directory reuses the pinned Pocket TTS and Kokoro asset preparation and
worker from ThreadShelf Local Suite commit
`2d7599b70bfad338658b75ba6aaef572f0d3fdb3`, preserving its model hashes,
voice presets, CPU-only inference, offline environment and telemetry opt-out.

- Pocket TTS release v4.0.1, native engine 3.3.0, English October 2026 100M
  model with six presets: Alba, Marius, Javert, Anna, Charles and Vera.
- Kokoro v1.0 82M FP32 ONNX model, kokoro-onnx 0.6.1, ONNX Runtime 1.31.1,
  bundled eSpeak NG and all 28 English presets.
- Bundled CPython 3.11.18; no system Python or GPU is required at runtime.

Reconstruct the payload using Python 3.12 or newer:

```sh
python vendor/tts/prepare_assets.py --output vendor/assets/tts --cache .cache/tts-downloads --platform all
```

Use `--platform linux-x64` or `--platform win32-x64` for a single platform.
Downloads are verified against `assets-manifest.json`. The support files are
verified against `support-manifest.json`. Preparation writes a complete
platform inventory. `packaging/stage_tts.py` merges common assets with only
the selected platform, verifies the inventory, copies the worker, and writes
`PACKAGED-ASSETS.json`. Release packaging checks the staged bytes again.
All component license notices and corresponding source archives included by
the upstream payload travel inside `resources/tts-assets/licenses` and
`resources/tts-assets/sources`.

For a local application build:

```sh
python packaging/stage_tts.py --assets vendor/assets/tts --platform linux --destination build/resources/tts-assets
python packaging/verify_tts.py --assets build/resources/tts-assets
```

The second command generates a real PCM WAV with each engine using the
packaged interpreter. It uses temporary text/audio/cache paths and removes
them on completion. The application worker uses the same JSON request/result
protocol. Assets are downloaded only during the explicit build step.
