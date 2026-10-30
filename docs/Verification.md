# Verification scope

## Version 1.1.1 verification scope

This patch focuses on full-label speech and dictation controls, centered dictation actions at compact and wider window sizes, and updated screenshots in both themes. Local geometry checks passed at 100%, 125%, 150% and 200% display scaling. CI records the Windows/Linux build results, focused tests and package checks for the exact submitted commit. A screenshot documents the interface; it does not establish provider compatibility or physical audio-device operation.

## Version 1.1.0 local validation baseline

The following local checks were recorded for version 1.1.0. They are a historical baseline, not a claim that a later commit has passed. Consult the corresponding CI run for each release's exact results.

| Local check | Result |
| --- | --- |
| Linux Release build, Qt 6.4.2 and pinned libsodium 1.0.22 | Passed |
| Windows x64 cross-build, Qt 6.8.3 and MinGW GCC 13 | Passed; native Windows execution remains pending |
| `textsegments`, `ttsclient`, `modelpacks`, `providerprofiles`, `credentialstore`, `ui_profiles` | All six focused groups passed |
| Credential integrity and rollback | Wrong passphrase, modified ciphertext, invalid parameters, profile/origin binding, independent replacement and damaged-file rollback covered |
| UI state and minimum size | Centered meter, Live text state, selected-profile locking and 680 × 540 layout covered |
| Public source and documentation scan | No supplied API keys or internal workspace references found |

The OpenAI Realtime adapter is tested against a deterministic local protocol fixture. No live OpenAI account request is claimed. Transcript matching compares the provider's reported text with the submitted text; it does not independently transcribe returned audio or establish pronunciation accuracy. Punctuation and numeric-format differences are deliberately rejected by the conservative check.

The Windows portable package has been cross-built and its layout, dependencies and bundled model hashes checked. This is not a native Windows runtime test. Startup, encrypted-key storage and audio-device operation still need validation on Windows.

No physical microphone, speaker or private speech server is available to these automated checks. The local UI checks establish layout and state behavior, not physical audio-device operation. Recognition remains local and existing model weights are unchanged.

A live OpenRouter request through the production TTS client was previously verified for 1.0.0 (`elevenlabs/eleven-flash-v2.5`, voice `george`, returned MP3). That historical result does not establish new OpenAI Realtime behavior or current account availability. Direct-provider and physical-device checks must be made on the intended setup.

## Automated and device checks

The repository contains focused automated checks for text segmentation, speech HTTP/WebSocket adapters, cancellation, optional-model import, encrypted-credential integrity and UI state transitions. Service fixtures use synthetic audio and deterministic responses; they verify client behavior rather than real voices or provider availability.

Run the standard checks with:

```sh
python3 vendor/tools/build_sodium.py --output build/deps/sodium --cc gcc --jobs 3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSPEECHDESK_BUILD_TESTS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure --timeout 90
```

Build/CI logs identify the tested commit, operating system and toolchain. A successful build is not a physical microphone/speaker test. The headless `ui_profiles` subset runs on Windows and Linux in CTest; the additional playback cases in the UI test executable require an actual or virtual audio output.

A live provider check establishes compatibility only for the endpoint, model, voice and options used in that request. A fixture or one provider's successful request does not establish compatibility with another service. API keys are not needed for standard CI and must never be included in logs or repository files.

Runtime release validation should confirm archive layout, included Qt/runtime dependencies, default model hashes, optional-model exclusion, executable metadata and the single release checksum file. Test microphone permission, recording and playback on the target desktop before relying on them.

This project does not claim exhaustive dependency auditing, universal service compatibility, comparative model accuracy, bit-for-bit build reproducibility or testing on every supported operating-system version.
