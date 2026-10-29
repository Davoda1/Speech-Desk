# Native engines and model provenance

SpeechDesk uses local CPU recognition engines and a native microphone/audio decoder. Python is used for building and packaging, not by the installed application. Runtime packages contain four default recognition models; Git and source archives exclude downloaded model weights and generated engine binaries.

## Default models

| Model | Weight file | Bytes | License |
| --- | --- | ---: | --- |
| Cactus Whistle | `whistle.cact` | 16,919,407 | Apache-2.0 |
| Whisper Tiny, multilingual Q5_1 | `ggml-tiny-q5_1.bin` | 32,152,673 | MIT |
| Whisper Base, multilingual Q5_1 | `ggml-base-q5_1.bin` | 59,707,625 | MIT |
| Whisper Small, multilingual Q5_1 | `ggml-small-q5_1.bin` | 190,085,487 | MIT |

The total is 298,865,192 bytes. Exact URLs, revisions and hashes are in `vendor/provenance/model-provenance.json` and `vendor/native-assets.json`. [Optional models](Optional-Models.md) are catalogued separately and are never bundled with the default application.

- Whistle: [Cactus-Compute/whistle, revision 741118687e1f008f06d1351a402a731274789790](https://huggingface.co/Cactus-Compute/whistle/tree/741118687e1f008f06d1351a402a731274789790).
- Whisper weights: [ggerganov/whisper.cpp, revision 5359861c739e955e79d9a303bcbc70fb988958b1](https://huggingface.co/ggerganov/whisper.cpp/tree/5359861c739e955e79d9a303bcbc70fb988958b1).

## Acquire and build recognition assets

On a Linux build host with Python 3.12+, CMake, a C compiler and archive tools:

```sh
python3 vendor/tools/bootstrap_native.py --platform all
```

The bootstrap script verifies pinned downloads, stages the four models, obtains the upstream engines and builds the native audio helper. Windows engine/helper cross-builds use Zig 0.14.1. Choose `--platform linux` or `--platform windows` to stage one target. `--models-only` downloads the default weights without building engines.

Assets are staged under `vendor/assets`. After building SpeechDesk, copy them beside the executable with:

```sh
cmake --build build --target stage_resources
```

Development-time downloads are explicit; the installed application does not invoke these scripts. Rebuilding pinned source is not a claim of bit-for-bit output across toolchains. Build receipts and release checksums identify the actual generated files.

## Components

**Cactus Needle / Whistle.** The pinned engine is [Cactus-Compute/needle3 revision d6c7d6a2e9655505e0f8e23339356dd1705900eb](https://huggingface.co/Cactus-Compute/needle3/tree/d6c7d6a2e9655505e0f8e23339356dd1705900eb), licensed under Apache-2.0. Linux uses the upstream executable. Windows embeds a UTF-8 application manifest using `vendor/tools/patch_needle_manifest.py`; the source manifest and modification notice are included. The adaptation supports Unicode paths on Windows 10 version 1903 and newer. Needle's complete inference source is not included here; the build acquires its pinned binary.

**whisper.cpp / GGML.** Source and release pins are recorded in `vendor/provenance/whisper-provenance.json`: version 1.9.5, release `b5454`, source commit `d1be6fde11ac6e0407606b4e42fe72d34add8037`. Linux stages the upstream CPU binaries, with runtime CPU dispatch and a baseline x64 variant. Windows builds a baseline executable with AVX, AVX2, FMA, F16C and BMI2 disabled, using a static C++ runtime and the system Universal CRT. SpeechDesk selects the baseline `whisper-cli.exe`.

**Native audio helper.** The MIT-licensed SpeechDesk helper in `vendor/native/whistle_audio.c` uses [miniaudio 0.11.25](https://github.com/mackron/miniaudio/tree/0.11.25). The pinned header and license are included. `vendor/provenance/audio-helper-build.json` describes its source and build parameters. The helper supports WASAPI, DirectSound and WinMM on Windows, and PulseAudio/ALSA on Linux. It decodes WAV, MP3 and FLAC to 16 kHz mono PCM and reports capture errors rather than substituting a dummy microphone.

**Qt and multimedia runtimes.** Packaging targets Qt 6.8.3 with dynamic linking. Packages include the required Qt libraries/plugins and multimedia runtime dependencies selected by the packaging scripts. OS graphics, fonts and audio services remain system dependencies. The Linux recognition engine requires at least glibc 2.34 and GLIBCXX 3.4.29; the packaged application and its toolchain may impose a newer baseline. Test the actual package on the intended distribution.

## Runtime behavior and privacy

Recognition is local CPU processing, with one recognition worker at a time. File input is limited to one hour and 1 GiB. Live recognition checks for captured audio periodically and does not guarantee real-time transcription. Cancellation terminates native workers and discards stale results.

SpeechDesk passes argument arrays to local worker processes, does not enable their server modes and does not download models at runtime. Worker environments set `NEEDLE_TELEMETRY=0`, `DO_NOT_TRACK=1` and `HF_HUB_OFFLINE=1`. Environment variables alone are not proof of a binary's behavior.

`vendor/provenance/native-privacy-inspection.json` records a static inspection of the exact upstream files identified by its hashes. It found no matching telemetry SDK or destination strings; Needle has optional loopback server code that SpeechDesk does not enable. This record is not dynamic network tracing or an exhaustive binary audit. Rebuilt binaries are covered only where their exact hashes are listed. OS audio services may have their own network configuration.

## Licenses

Application and native-helper code use MIT. Models and engines retain their upstream MIT/Apache notices; Qt, FFmpeg and compiler/runtime dependencies retain their respective notices in `vendor/licenses/`. Runtime packages copy the applicable notices alongside their bundled dependencies. The application's MIT license does not replace or restrict these third-party licenses.
