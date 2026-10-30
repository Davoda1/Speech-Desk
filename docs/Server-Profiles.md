# SpeechDesk server profiles

Profiles describe how SpeechDesk connects to a speech service. They do not install a model, start a server, or make a text-chat endpoint synthesize audio. Use **Settings → Servers** for ordinary connection fields and **Advanced…** for documented JSON mappings.

## Hosted providers

Choose a preset in **Settings → Servers**. Enter your own provider API key in the dedicated field. Text sent for synthesis is processed by the chosen service under its retention and billing terms. Generated playback is AI-generated speech.

| Provider | Base address | Speech endpoint | Authentication |
| --- | --- | --- | --- |
| OpenAI Realtime | `https://api.openai.com` | `/v1/realtime` over secure WebSocket | Bearer API key |
| OpenAI legacy speech | `https://api.openai.com` | `/v1/audio/speech` | Bearer API key |
| ElevenLabs | `https://api.elevenlabs.io` | `/v1/text-to-speech/${voice}` | `xi-api-key` |
| OpenRouter | `https://openrouter.ai` | `/api/v1/audio/speech` | Bearer API key |

Select a model and voice accepted by that provider. ElevenLabs uses `model_id` in its request and a voice ID in the endpoint; its default output is `mp3_44100_128`. OpenRouter lists speech-capable models through its Models API. Models may expose different voices, formats and optional controls. Discovery can be unavailable or incomplete; manually entered IDs remain supported.

**OpenAI model lifecycle:** The OpenAI preset defaults to `gpt-realtime-2.1-mini` with voice `marin`. OpenAI retires `tts-1`, `tts-1-hd`, and `gpt-4o-mini-tts`, including their dated versions, on **6 January 2027**. On and after that UTC date, SpeechDesk excludes these legacy models from official OpenAI discovery and refuses their synthesis requests. This rule is limited to the official `api.openai.com` endpoint; compatible self-hosted services are unaffected. See [OpenAI's deprecation schedule](https://developers.openai.com/api/docs/deprecations).

Official references: [OpenAI speech](https://developers.openai.com/api/docs/guides/text-to-speech), [ElevenLabs speech](https://elevenlabs.io/docs/api-reference/text-to-speech/convert), [OpenRouter speech](https://openrouter.ai/docs/guides/overview/multimodal/tts).

## OpenAI Realtime

Selecting a supported `gpt-realtime-2.1` or `gpt-realtime-2.1-mini` model, including a dated version, on the OpenAI provider routes synthesis through the Realtime adapter. The profile keeps its HTTPS base address; SpeechDesk uses a secure WebSocket for the request. `realtimePath` defaults to `/v1/realtime`. The model is sent as a query parameter and the key as a Bearer authorization header. The `openai` adapter ID remains valid; the selected model determines the protocol.

For each text section, the client establishes a session, sends the unchanged source text as a text message, and requests audio output. It does not upload microphone recordings or enable conversational turn detection. Tool use is disabled. Output is buffered as 24 kHz mono PCM and delivered for playback only after the section completes and its final transcript passes validation. Realtime output uses section highlighting, without estimated word timings.

Transcript comparison normalizes Unicode to NFC, collapses whitespace, and ignores letter case. It does not discard punctuation, rewrite numbers, or treat different wording as equivalent. Missing, incomplete, or mismatched output produces an error and is not played. This conservative comparison can reject acceptable pronunciation when the returned transcript uses different punctuation or numeric notation; it is not an independent verification of the audio itself.

The HTTP request-template, `extraBody`, and response-mapping options below do not alter this fixed Realtime exchange. Its controls are the model, voice, `realtimePath`, `timeoutSeconds`, and optional `sendInstructions` / `instructions` for delivery guidance. The request uses the configured timeout and a 4,096 output-token limit per section; incomplete responses are rejected without playing partial audio. Audio remains limited to 64 MiB per section, and individual incoming Realtime events are limited to 4 MiB. Arbitrary WebSocket protocols, conversational microphone sessions, and custom Realtime tools are not implemented by this adapter.

## Self-hosted and compatible services

Enter the server's origin, such as `http://localhost:8000` or `https://speech.example.org`. These are illustrative addresses, not preconfigured destinations. The default speech path is `/v1/audio/speech`. For self-hosted compatible adapters, an optional API key is sent as `Authorization: Bearer …`.

**Automatic** checks `/openapi.json`, `/v1/models` and `/v1/audio/voices` on the configured origin. It can suggest the OpenAI-compatible, vLLM-Omni or Kokoro adapter from the advertised speech endpoint and schema. Detection is also performed on first Speak when the matching address/path has no saved discovery result. **Detect capabilities** refreshes the information manually.

A missing discovery endpoint is not a definitive failure: choose the connection type manually and enter the documented model/voice IDs. Discovery does not automatically infer arbitrary request formats, start server models or prove that every advertised feature works.

## Core fields

| Field | Meaning / default |
| --- | --- |
| `name`, `id` | Display name and profile identity |
| `baseUrl` | HTTP/HTTPS server address; no embedded credentials, query or fragment |
| `adapter` | `auto`, `openai`, `openrouter`, `elevenlabs`, `vllm`, `kokoro`, or `custom` |
| `apiKey` | Runtime provider API key (Bearer or `xi-api-key`); never included in saved settings or exports |
| `model`, `voice` | Server's model and predefined/saved voice IDs |
| `speechPath` | `/v1/audio/speech` for compatible HTTP speech APIs |
| `realtimePath` | `/v1/realtime` for supported OpenAI Realtime models |
| `openApiPath` | `/openapi.json` |
| `modelsPath`, `voicesPath` | `/v1/models`, `/v1/audio/voices` |
| `maxChars` | Section limit: 80–8,000; default 500 |
| `timeoutSeconds` | Request timeout; Settings permits 10–1,800 seconds; default 180 |
| `responseFormat` | Requested format; default `wav` |
| `responseMode` | `auto`, `binary`, `json`, or `sse` |

A path beginning with `/` is relative to the origin root. A relative path without `/` is resolved beneath the base URL's path. For example, base URL `https://host/service/` and speech path `speech` resolve to `/service/speech`; speech path `/v1/audio/speech` resolves to the origin's `/v1/audio/speech`.

For HTTP OpenAI-compatible and self-hosted adapters, the request contains `input`, optional nonempty `model` and `voice`, and `response_format`. The vLLM adapter also sends nonempty `language` and `instructions`. Other adapters can enable those two fields with `sendLanguage: true` and `sendInstructions: true`. Add only parameters documented by your service.

## Custom JSON request

`requestTemplate` accepts a JSON object or a string containing one. Placeholders inside JSON strings are replaced safely, including inside nested objects and arrays. Supported variables are exactly **`${text}`**, **`${model}`**, **`${voice}`**, **`${language}`**, and **`${instructions}`**.

```json
{
  "name": "Custom speech service",
  "baseUrl": "http://localhost:8000",
  "adapter": "custom",
  "speechPath": "/synthesize",
  "model": "server-model-id",
  "voice": "server-voice-id",
  "requestTemplate": {
    "text": "${text}",
    "speaker": "${voice}",
    "model_id": "${model}",
    "output": {"format": "wav"}
  },
  "responseMode": "json",
  "audioField": "result.audio",
  "audioEncoding": "base64",
  "timingsField": "result.words"
}
```

This illustrates mapping syntax; it is not a promise that a particular service uses these fields. `extraBody` adds fixed top-level JSON fields **after** template/default construction and overwrites matching fields. It does not expand placeholders. `headers` adds custom HTTP headers; invalid names, newline-containing values and transport-owned headers are rejected or ignored.

## Audio responses

| Response | Configuration |
| --- | --- |
| Raw WAV, MP3, FLAC or Ogg | `responseMode: "binary"`, or `auto` with an audio content type |
| Raw PCM | `responseFormat: "pcm"`; signed 16-bit little-endian samples |
| JSON base64 audio | `responseMode: "json"`, `audioEncoding: "base64"`, mapped `audioField` |
| JSON audio URL | `audioEncoding: "url"`; URL must stay on the configured origin |
| Speech-audio SSE | `responseMode: "sse"`, or `auto` with `text/event-stream` |

JSON field paths use dot notation; numeric segments address arrays, such as `results.0.audio`. The default audio field is `audio`. Common `audio_base64`, `data.audio`, `audio_url` and `url` fields are also recognized. Base64 data URLs are supported. JSON may specify `format` or `response_format`.

PCM uses `sampleRate` and `channels` from the profile unless supplied by recognized response metadata. Defaults are **24,000 Hz** and **one channel**; allowed values are 8,000–192,000 Hz and 1–8 channels. Set these correctly: raw PCM contains no self-describing header.

SSE supports JSON `data:` events containing base64 `audio` or `delta` values. Events may provide `response_format`, `sample_rate` and `channels`. Audio deltas are concatenated as one encoded audio stream or PCM stream, **buffered until the section response completes**, then played. Separate self-contained WAV files per event are not a supported concatenation format. `[DONE]` is accepted. This is not an arbitrary SSE-protocol interpreter or low-latency incremental player.

Responses are limited to 64 MiB per request; JSON requests to 2 MiB. Lower `maxChars` if a service's limits are smaller.

## Word highlighting

Set `timingsField` to an array of word records; its default is `words`:

```json
{
  "words": [
    {"word": "Hello", "start": 0.0, "end": 0.32},
    {"word": "world", "start": 0.33, "end": 0.75}
  ]
}
```

`text` can replace `word`. `start`/`end` are seconds; paired `start_ms`/`end_ms` are milliseconds. Times must refer to the complete returned section's audio. For SSE, complete-section timing records are read from the `speech.audio.done` event, not from individual delta events with ambiguous time origins.

Recognized labels are matched to the original request text in sequence. Invalid/unmatched records are skipped. With no usable cues, the application highlights the actual currently playing text section. It never substitutes an estimated word clock.

## Credentials, profiles and limits

Plaintext API keys and standard credential headers are removed from settings and exported profiles. Optional passphrase-encrypted key storage is managed separately for each profile; exports do not contain the encrypted credential record or its passphrase. See [Saved API keys](User-Guide.md#saved-api-keys). Redacted headers are `Authorization`, `Proxy-Authorization`, `X-API-Key`, `API-Key`, `xi-api-key` and `Cookie`, regardless of case. Other custom headers and all custom body/template fields can persist. Do not put credentials there. Imported profiles should be reviewed before use.

URLs and endpoints must stay on the configured origin: the same scheme, hostname and effective port. Returned audio URLs follow the same rule. Redirects are not followed; enter the final address. Standard TLS certificate checks remain enabled.

Voice cloning, reference uploads, arbitrary WebSocket protocols, gRPC, asynchronous job polling and unknown future protocols are outside this release's adapters. The specific OpenAI Realtime WebSocket protocol described above is supported. They require additional code or a compatible HTTP gateway. Relevant server documentation includes the [vLLM-Omni speech API](https://docs.vllm.ai/projects/vllm-omni/en/latest/serving/speech_api/) and [Kokoro-FastAPI upstream README](https://github.com/remsky/Kokoro-FastAPI). A server-side feature appearing in those documents does not by itself mean this client implements it.
