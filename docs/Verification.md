# Verification scope

## Recorded checks for 1.0.0

| Check | Result and scope |
| --- | --- |
| Native Linux Release build | Passed with Qt 6.4.2 |
| Standard CTest groups | `textsegments`, `ttsclient`, `modelpacks`, `providerprofiles` and the Linux `ui_profiles` subset passed |
| Focused UI checks | Model-specific OpenRouter voices, minimum-size dictation layout, saved profile edits and text modified-state behavior passed with the FFmpeg media backend |
| Live OpenRouter request | Discovery returned speech models; `elevenlabs/eleven-flash-v2.5` with voice `george` returned MP3 audio through the production TTS client |
| Returned audio inspection | MP3 decoded as 44.1 kHz mono, approximately 3.47 seconds |

The OpenRouter request tests ElevenLabs routing through OpenRouter. It does not establish direct ElevenLabs or OpenAI account integration; no live direct-provider requests are included in these results. No physical microphone or speaker was available for these checks. Windows execution and final release-package validation are not established by this Linux build.

## Automated and device checks

The repository contains focused automated checks for text segmentation, speech HTTP adapters, cancellation and optional-model import. Service fixtures use synthetic audio and deterministic responses; they verify client behavior rather than real voices or provider availability.

Run the standard checks with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSPEECHDESK_BUILD_TESTS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure --timeout 90
```

Build/CI logs identify the tested commit, operating system and toolchain. A successful build is not a physical microphone/speaker test. The headless `ui_profiles` subset runs on Linux in CTest; the additional playback cases in the UI test executable require an actual or virtual audio output.

A live provider check establishes compatibility only for the endpoint, model, voice and options used in that request. A fixture or one provider's successful request does not establish compatibility with another service. API keys are not needed for standard CI and must never be included in logs or repository files.

Runtime release validation should confirm archive layout, included Qt/runtime dependencies, default model hashes, optional-model exclusion, executable metadata and the single release checksum file. Test microphone permission, recording and playback on the target desktop before relying on them.

This project does not claim exhaustive dependency auditing, universal service compatibility, comparative model accuracy, bit-for-bit build reproducibility or testing on every supported operating-system version.
