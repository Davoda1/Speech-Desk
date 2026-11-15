#!/usr/bin/env python3
"""Portable, offline CPU adapters for Pocket TTS and Kokoro.

CLI: read one JSON request from stdin and emit JSONL progress/result events.
Set PYTHONPATH to the matching portable runtime/packages/light directory.
Model and native-engine roots are explicit; nothing is downloaded at inference.
"""
from __future__ import annotations

import argparse
import base64
import contextlib
import json
import logging
import os
# ONNX Runtime 1.31.1 can initialize its cross-platform telemetry provider
# during import, before disable_telemetry_events() can run. The official
# docs/Privacy.md opt-out must therefore be set before any third-party import.
os.environ["ORT_DISABLE_TELEMETRY"] = "1"
from pathlib import Path
import queue
import re
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid
import wave
from typing import Callable


POCKET_VOICES = ("alba", "marius", "javert", "anna", "charles", "vera")
KOKORO_VOICES = (
    "af_heart", "af_alloy", "af_aoede", "af_bella", "af_jessica", "af_kore",
    "af_nicole", "af_nova", "af_river", "af_sarah", "af_sky", "am_adam",
    "am_echo", "am_eric", "am_fenrir", "am_liam", "am_michael", "am_onyx",
    "am_puck", "am_santa", "bf_alice", "bf_emma", "bf_isabella", "bf_lily",
    "bm_daniel", "bm_fable", "bm_george", "bm_lewis",
)
DEFAULT_VOICES = {"pocket": "alba", "kokoro": "af_heart"}
_ACTIVE_NATIVE: subprocess.Popen | None = None
MAX_AUDIO_BYTES = 64 * 1024 * 1024
STREAM_CHUNK_BYTES = 24000  # 500 ms, mono PCM16 at 24 kHz.


class _PCMStream:
    """Write a complete WAV and emit bounded, ordered PCM chunks concurrently."""
    def __init__(self, output: Path, callback):
        self.writer = wave.open(str(output), "wb")
        self.writer.setnchannels(1)
        self.writer.setsampwidth(2)
        self.writer.setframerate(24000)
        self.callback = callback
        self.pending = bytearray()
        self.bytes = 0
        self.index = 0
        self.section = {}

    def __enter__(self):
        return self

    def _emit(self, data: bytes):
        self.callback({"type": "audio", "index": self.index,
                       "pcmBase64": base64.b64encode(data).decode("ascii"),
                       "sample_rate": 24000, "channels": 1, "sample_width": 2,
                       **self.section})
        self.index += 1

    def write(self, data: bytes, **section):
        if len(data) % 2:
            raise RuntimeError("The speech engine returned an incomplete PCM frame")
        if self.bytes + len(data) + 44 > MAX_AUDIO_BYTES:
            raise RuntimeError("The speech engine returned oversized audio")
        self.writer.writeframesraw(data)
        self.bytes += len(data)
        self.pending.extend(data)
        self.section = section
        while len(self.pending) >= STREAM_CHUNK_BYTES:
            self._emit(bytes(self.pending[:STREAM_CHUNK_BYTES]))
            del self.pending[:STREAM_CHUNK_BYTES]

    def __exit__(self, kind, error, traceback):
        try:
            if kind is None and self.pending:
                self._emit(bytes(self.pending))
            self.pending.clear()
        finally:
            self.writer.close()


def _speech_segments(text: str, limit: int = 180) -> list[str]:
    """Prefer sentence/clause boundaries while bounding first-audio latency."""
    units = re.split(r"(?<=[.!?])\s+|\n+", text.strip())
    segments = []
    for unit in units:
        unit = unit.strip()
        while len(unit) > limit:
            window = unit[:limit + 1]
            clauses = list(re.finditer(r"[,;:]\s+", window))
            cut = clauses[-1].end() if clauses and clauses[-1].end() >= limit // 2 else window.rfind(" ")
            if cut < limit // 3:
                cut = limit
            segments.append(unit[:cut].strip())
            unit = unit[cut:].strip()
        if unit:
            # Avoid isolated abbreviations such as Dr. without postponing a
            # normal first sentence while waiting to fill a fixed-size batch.
            if segments and len(segments[-1]) < 25 and len(segments[-1]) + len(unit) + 1 <= limit:
                segments[-1] += " " + unit
            else:
                segments.append(unit)
    return segments


def _set_environment(cache_root: Path, threads: int) -> None:
    """Set library caches before imports without redirecting the user's home."""
    cache_root.mkdir(parents=True, exist_ok=True)
    tmp = cache_root / "tmp"
    tmp.mkdir(parents=True, exist_ok=True)
    values = {
        "HF_HOME": str(cache_root / "huggingface"),
        "HF_HUB_CACHE": str(cache_root / "huggingface" / "hub"),
        "HF_HUB_OFFLINE": "1",
        "HF_HUB_DISABLE_TELEMETRY": "1",
        "TRANSFORMERS_OFFLINE": "1",
        "XDG_CACHE_HOME": str(cache_root),
        "TMPDIR": str(tmp), "TMP": str(tmp), "TEMP": str(tmp),
        "PYTHONDONTWRITEBYTECODE": "1",
        "ONNX_PROVIDER": "CPUExecutionProvider",
        "ORT_DISABLE_TELEMETRY": "1",
        "CUDA_VISIBLE_DEVICES": "",
        "OMP_NUM_THREADS": str(threads),
        "MKL_NUM_THREADS": str(threads),
        "OPENBLAS_NUM_THREADS": str(threads),
        "RAYON_NUM_THREADS": str(threads),
    }
    os.environ.update(values)
    tempfile.tempdir = str(tmp)
    sys.dont_write_bytecode = True


def _session(model: Path, threads: int):
    import onnxruntime as ort
    if ort.__version__ != "1.31.1":
        raise RuntimeError("The bundled ONNX Runtime version does not match the release lock (1.31.1)")
    ort.disable_telemetry_events()
    options = ort.SessionOptions()
    options.intra_op_num_threads = threads
    options.inter_op_num_threads = 1
    options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
    options.log_severity_level = 3
    # Avoid continuous spinning while the GUI is idle between calls.
    options.add_session_config_entry("session.intra_op.allow_spinning", "0")
    options.add_session_config_entry("session.inter_op.allow_spinning", "0")
    session = ort.InferenceSession(str(model), sess_options=options, providers=["CPUExecutionProvider"])
    if session.get_providers() != ["CPUExecutionProvider"]:
        raise RuntimeError("The inference session did not select the CPU-only provider")
    return session


def _pcm_bytes(audio):
    import numpy as np
    data = np.asarray(audio, dtype=np.float32).reshape(-1)
    if not np.isfinite(data).all():
        raise RuntimeError("The model returned non-finite audio samples")
    return (np.clip(data, -1, 1) * 32767).astype("<i2").tobytes()


def _wave_info(path: Path) -> dict:
    with wave.open(str(path), "rb") as reader:
        rate = reader.getframerate()
        frames = reader.getnframes()
        if not frames or not rate:
            raise RuntimeError("The engine produced empty audio")
        return {
            "sample_rate": rate,
            "duration_seconds": frames / rate,
            "channels": reader.getnchannels(),
            "sample_width": reader.getsampwidth(),
            "frames": frames,
        }


@contextlib.contextmanager
def _relocatable_espeak(data_root: Path):
    """Keep eSpeak's native data path short, including deeply nested installs.

    eSpeak 1.52 has a 160-byte Linux/230-byte Windows path_home buffer and
    silently falls back to its build-machine path when an absolute data path
    exceeds it. Phonemizer resolves configured paths to absolute paths, so a
    relative EspeakConfig alone cannot fix this. This per-request worker uses
    the bundled, read-only data directory as CWD and passes '.' only at the
    native API boundary. Model, output and temporary paths remain absolute.
    """
    from phonemizer.backend.espeak import wrapper
    data_root = data_root.resolve()
    if not (data_root / "phontab").is_file():
        raise RuntimeError("The bundled eSpeak data is incomplete")
    previous_directory = Path.cwd()
    previous_api = wrapper.EspeakAPI

    class RelativeDataAPI(previous_api):
        def __init__(self, library, data_path):
            if data_path is None or Path(data_path).resolve() != data_root:
                raise RuntimeError("The speech engine requested a non-bundled eSpeak data directory")
            super().__init__(library, ".")

    try:
        os.chdir(data_root)
        wrapper.EspeakAPI = RelativeDataAPI
        yield
    finally:
        wrapper.EspeakAPI = previous_api
        os.chdir(previous_directory)


def _kokoro(text, voice, threads, output, models, engines, cache, progress, audio_callback=None):
    import espeakng_loader
    with _relocatable_espeak(Path(espeakng_loader.get_data_path())):
        return _kokoro_render(text, voice, threads, output, models, engines, cache, progress, audio_callback)


def _kokoro_render(text, voice, threads, output, models, engines, cache, progress, audio_callback=None):
    started = time.perf_counter()
    from kokoro_onnx import Kokoro
    from kokoro_onnx.config import EspeakConfig
    import espeakng_loader
    if voice not in KOKORO_VOICES:
        raise ValueError(f"Unknown bundled English Kokoro voice: {voice}")
    # Force the packaged eSpeak library rather than a user/system override.
    os.environ["PHONEMIZER_ESPEAK_LIBRARY"] = espeakng_loader.get_library_path()
    espeak = EspeakConfig(lib_path=espeakng_loader.get_library_path(), data_path=espeakng_loader.get_data_path())
    model_dir = models / "kokoro"
    speaker = Kokoro.from_session(_session(model_dir / "kokoro-v1.0.onnx", threads), str(model_dir / "voices-v1.0.bin"), espeak_config=espeak)
    if voice not in speaker.get_voices():
        raise ValueError(f"The voice pack does not contain {voice}")
    loaded = time.perf_counter()
    progress(stage="generating", message="Generating with Kokoro 82M, full precision", load_seconds=loaded-started)
    language = "en-gb" if voice.startswith("b") else "en-us"
    # Keep ONNX inference on its creating thread. Kokoro's asynchronous helper
    # stalled with this portable ORT build; the synchronous API uses the same
    # phoneme batching and preserves the model's full-precision output.
    if audio_callback is None:
        audio, rate = speaker.create(text, voice=voice, speed=1.0, lang=language)
        if rate != 24000:
            raise RuntimeError(f"Unexpected Kokoro sample rate: {rate}")
        first_audio = time.perf_counter()-started
        with wave.open(str(output), "wb") as writer:
            writer.setnchannels(1)
            writer.setsampwidth(2)
            writer.setframerate(rate)
            writer.writeframes(_pcm_bytes(audio))
        audio_seconds = len(audio)/rate
        chunks = 1
    else:
        segments = _speech_segments(text)
        first_audio = None
        with _PCMStream(output, audio_callback) as writer:
            for index, segment in enumerate(segments):
                # This one model/session stays on its creating thread for the
                # whole utterance. Later sentences run while earlier PCM plays.
                audio, rate = speaker.create(segment, voice=voice, speed=1.0, lang=language)
                if rate != 24000:
                    raise RuntimeError(f"Unexpected Kokoro sample rate: {rate}")
                if first_audio is None:
                    first_audio = time.perf_counter()-started
                writer.write(_pcm_bytes(audio), section=index + 1, sections=len(segments))
                if index + 1 < len(segments):
                    pause = 0.22 if segment.rstrip().endswith((".", "!", "?")) else 0.10
                    writer.write(b"\0" * (int(pause * 24000) * 2), section=index + 1, sections=len(segments))
                progress(stage="generating", message="Generating with Kokoro 82M, full precision",
                         section=index + 1, sections=len(segments), generated_audio_seconds=writer.bytes/48000)
            audio_seconds = writer.bytes/48000
        chunks = writer.index
    progress(stage="generating", message="Kokoro audio complete", generated_audio_seconds=audio_seconds)
    return {"load_seconds": loaded-started, "synthesis_seconds": time.perf_counter()-loaded,
            "first_audio_seconds": first_audio, "first_audio_measurement": "first sentence PCM" if audio_callback else "complete synchronous render",
            "chunks": chunks, "model_loads": 1,
            "engine_version": "kokoro-onnx 0.6.1", "model_version": "82M v1.0", "precision": "float32", "provider": "CPUExecutionProvider", "phonemizer": "bundled eSpeak NG"}


def _die_with_parent():
    """Linux: prevent an interrupted Python worker from orphaning native TTS."""
    import ctypes
    libc = ctypes.CDLL(None)
    libc.prctl(1, signal.SIGTERM)
    if os.getppid() == 1:
        os.kill(os.getpid(), signal.SIGTERM)


def _pocket_stream(text, voice, threads, output, models, engines, cache, progress, audio_callback):
    """Consume the bundled native engine's real streaming WAV stdout."""
    global _ACTIVE_NATIVE
    started = time.perf_counter()
    if voice not in POCKET_VOICES:
        raise ValueError(f"Unknown bundled Pocket voice: {voice}")
    model_dir = models / "pocket"
    executable = engines / "pocket" / ("pocket-tts.exe" if os.name == "nt" else "pocket-tts")
    voice_path = model_dir / "voices" / (voice + ".safetensors")
    for required in [executable, voice_path, model_dir / "model.safetensors", model_dir / "tokenizer.json", model_dir / "english_2026-10.upstream.yaml"]:
        if not required.is_file():
            raise FileNotFoundError(f"Pocket asset is missing: {required}")
    yaml = (model_dir / "english_2026-10.upstream.yaml").read_text(encoding="utf-8")
    quote = lambda value: "'" + str(value).replace("'", "''") + "'"
    weights = quote(model_dir / "model.safetensors")
    tokenizer = quote(model_dir / "tokenizer.json")
    yaml = re.sub(r"(?m)^weights_path:.*$", lambda _: "weights_path: " + weights, yaml)
    yaml = re.sub(r"(?m)^weights_path_without_voice_cloning:.*$", lambda _: "weights_path_without_voice_cloning: " + weights, yaml)
    yaml = re.sub(r"(?m)^(\s*)tokenizer_path:.*$", lambda match: match.group(1) + "tokenizer_path: " + tokenizer, yaml)
    with tempfile.TemporaryDirectory(prefix="pocket-", dir=cache / "tmp") as work:
        config = Path(work) / "model.yaml"
        config.write_text(yaml, encoding="utf-8")
        command = [str(executable), "generate", "--text", "-", "--config", str(config),
                   "--voice", str(voice_path), "--output-path", "-", "--device", "cpu", "--quiet"]
        options = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
        if sys.platform.startswith("linux"):
            options["preexec_fn"] = _die_with_parent
        environment = os.environ.copy()
        environment["FIRST_CHUNK_LENGTH_SECONDS"] = "0"
        proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, bufsize=0, cwd=work,
                                env=environment, **options)
        _ACTIVE_NATIVE = proc
        diagnostic_bytes = 0
        diagnostic_error = []

        def collect_diagnostics():
            nonlocal diagnostic_bytes
            try:
                while data := proc.stderr.read(4096):
                    diagnostic_bytes += len(data)
                    if diagnostic_bytes > 2 * 1024 * 1024:
                        diagnostic_error.append("Pocket returned too much diagnostic output")
                        proc.terminate()
                        return
            except (OSError, ValueError):
                pass

        reader = threading.Thread(target=collect_diagnostics, daemon=True)
        reader.start()
        first_audio = None
        try:
            encoded = memoryview(text.encode("utf-8"))
            while encoded:
                written = proc.stdin.write(encoded)
                if not written:
                    raise RuntimeError("Pocket closed its text input before receiving the passage")
                encoded = encoded[written:]
            proc.stdin.close()
            header = bytearray()
            while len(header) < 44:
                data = proc.stdout.read(44 - len(header))
                if not data:
                    raise RuntimeError("Pocket returned an incomplete streaming WAV header")
                header.extend(data)
            # This pinned native runtime writes the standard 44-byte PCM16
            # header. Its pipe length is deliberately unknown until EOF.
            if (header[:4] != b"RIFF" or header[8:16] != b"WAVEfmt " or
                struct.unpack_from("<IHHIIHH", header, 16) != (16, 1, 1, 24000, 48000, 2, 16) or
                header[36:40] != b"data"):
                raise RuntimeError("Pocket returned an unsupported streaming audio format")
            with _PCMStream(output, audio_callback) as writer:
                remainder = b""
                while data := proc.stdout.read(STREAM_CHUNK_BYTES):
                    if first_audio is None:
                        first_audio = time.perf_counter()-started
                    data = remainder + data
                    complete = len(data) - len(data) % 2
                    if complete:
                        writer.write(data[:complete])
                    remainder = data[complete:]
                if remainder:
                    raise RuntimeError("Pocket returned an incomplete PCM frame")
                code = proc.wait(timeout=5)
                reader.join(timeout=1)
                if diagnostic_error:
                    raise RuntimeError(diagnostic_error[0])
                if code:
                    raise RuntimeError(f"Pocket speech generation failed (exit {code})")
                if not writer.bytes:
                    raise RuntimeError("Pocket returned empty audio")
                audio_seconds = writer.bytes/48000
            chunks = writer.index
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=3)
            for pipe in (proc.stdin, proc.stdout, proc.stderr):
                if pipe is not None:
                    pipe.close()
            reader.join(timeout=1)
            _ACTIVE_NATIVE = None
    progress(stage="generating", message="Pocket audio complete", generated_audio_seconds=audio_seconds)
    return {"load_seconds": None, "synthesis_seconds": time.perf_counter()-started,
            "first_audio_seconds": first_audio, "first_audio_measurement": "native streaming PCM",
            "chunks": chunks, "model_loads": 1,
            "engine_version": "official release v4.0.1 (native asset 3.3.0)",
            "model_version": "English 100M, 2026-10", "precision": "float32 inference", "provider": "native CPU"}


def _pocket(text, voice, threads, output, models, engines, cache, progress, audio_callback=None):
    if audio_callback is not None:
        return _pocket_stream(text, voice, threads, output, models, engines, cache, progress, audio_callback)
    global _ACTIVE_NATIVE
    started = time.perf_counter()
    if voice not in POCKET_VOICES:
        raise ValueError(f"Unknown bundled Pocket voice: {voice}")
    model_dir = models / "pocket"
    executable = engines / "pocket" / ("pocket-tts.exe" if os.name == "nt" else "pocket-tts")
    if not executable.is_file():
        raise FileNotFoundError(f"Pocket native runtime is missing: {executable}")
    voice_path = model_dir / "voices" / (voice + ".safetensors")
    required = [voice_path, model_dir / "model.safetensors", model_dir / "tokenizer.json", model_dir / "english_2026-10.upstream.yaml"]
    for path in required:
        if not path.is_file():
            raise FileNotFoundError(f"Pocket asset is missing: {path}")
    yaml = required[-1].read_text(encoding="utf-8")
    # YAML single-quoted paths support Windows drive letters, backslashes,
    # spaces and apostrophes without interpreting escape sequences.
    quote = lambda path: "'" + str(path).replace("'", "''") + "'"
    weights = quote(model_dir / "model.safetensors")
    tokenizer = quote(model_dir / "tokenizer.json")
    yaml = re.sub(r"(?m)^weights_path:.*$", lambda _: "weights_path: " + weights, yaml)
    yaml = re.sub(r"(?m)^weights_path_without_voice_cloning:.*$", lambda _: "weights_path_without_voice_cloning: " + weights, yaml)
    yaml = re.sub(r"(?m)^(\s*)tokenizer_path:.*$", lambda match: match.group(1) + "tokenizer_path: " + tokenizer, yaml)
    with tempfile.TemporaryDirectory(prefix="pocket-", dir=cache / "tmp") as work:
        config = Path(work) / "model.yaml"
        config.write_text(yaml, encoding="utf-8")
        command = [str(executable), "generate", "--text", "-", "--config", str(config),
                   "--voice", str(voice_path), "--output-path", str(output), "--device", "cpu"]
        options = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
        if sys.platform.startswith("linux"):
            options["preexec_fn"] = _die_with_parent
        proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                                errors="replace", cwd=work, env=os.environ.copy(), **options)
        _ACTIVE_NATIVE = proc
        lines = queue.Queue()

        def collect():
            for line in proc.stdout:
                lines.put(line.rstrip())

        reader = threading.Thread(target=collect, daemon=True)
        reader.start()
        logs = []
        first_audio = None
        native_synthesis = None
        last_progress = 0.0
        try:
            proc.stdin.write(text)
            proc.stdin.close()
            while proc.poll() is None or not lines.empty():
                try:
                    line = lines.get(timeout=0.1)
                    logs.append(line)
                    if len(logs) > 60:
                        logs.pop(0)
                    match = re.search(r"Generated: ([0-9.]+) ms of audio in ([0-9.]+) ms", line)
                    if match:
                        native_synthesis = float(match.group(2)) / 1000
                except queue.Empty:
                    pass
                now = time.perf_counter()
                if output.exists() and output.stat().st_size > 44 and first_audio is None:
                    first_audio = now-started
                if now-last_progress > 0.5:
                    progress(stage="generating", message="Generating with Pocket TTS 100M", native_elapsed_seconds=now-started)
                    last_progress = now
            reader.join(timeout=1)
            while not lines.empty():
                line = lines.get_nowait()
                logs.append(line)
                match = re.search(r"Generated: ([0-9.]+) ms of audio in ([0-9.]+) ms", line)
                if match:
                    native_synthesis = float(match.group(2))/1000
            if proc.returncode:
                raise RuntimeError("Pocket TTS failed (exit " + str(proc.returncode) + "): " + "\n".join(logs[-12:]))
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
            _ACTIVE_NATIVE = None
    return {"load_seconds": None, "synthesis_seconds": native_synthesis,
            "first_audio_seconds": first_audio, "first_audio_measurement": "output-file polling, approximate",
            "engine_version": "official release v4.0.1 (native asset 3.3.0)",
            "model_version": "English 100M, 2026-10", "precision": "float32 inference", "provider": "native CPU"}


def generate(engine: str, text: str, voice: str | None, threads: int,
             output_path: str | Path, model_root: str | Path,
             progress_callback: Callable[[dict], None] | None = None,
             *, engine_root: str | Path | None = None,
             cache_root: str | Path | None = None,
             streaming: bool = False) -> dict:
    """Generate one PCM16 WAV using explicit local assets and CPU inference.

    model_root contains pocket/ and kokoro/; engine_root contains
    pocket/pocket-tts[.exe]. cache_root must be within the portable app's data
    directory. If omitted it is a sibling of model_root called cache/light.
    Timings describe this cold worker invocation, not persistent-server latency.
    """
    if engine not in DEFAULT_VOICES:
        raise ValueError(f"Unknown engine: {engine}")
    if not isinstance(text, str) or not text.strip():
        raise ValueError("Enter some text to synthesize")
    if isinstance(threads, bool) or not 1 <= int(threads) <= 64:
        raise ValueError("threads must be an integer between 1 and 64")
    threads = int(threads)
    voice = voice or DEFAULT_VOICES[engine]
    models = Path(model_root).resolve()
    engines = Path(engine_root).resolve() if engine_root is not None else models.parent / "engines"
    cache = Path(cache_root).resolve() if cache_root is not None else models.parent / "cache" / "light"
    output = Path(output_path).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    _set_environment(cache, threads)
    started = time.perf_counter()

    def progress(**values):
        if progress_callback:
            progress_callback({"type": "progress", "engine": engine, "elapsed_seconds": time.perf_counter()-started, **values})

    def audio(event):
        if progress_callback:
            progress_callback({**event, "engine": engine, "elapsed_seconds": time.perf_counter()-started})

    progress(stage="loading", message=f"Loading {engine} on CPU", threads=threads)
    partial = output.with_name(output.stem + "." + uuid.uuid4().hex[:10] + ".partial.wav")
    try:
        implementation = {"pocket": _pocket, "kokoro": _kokoro}[engine]
        metadata = implementation(text, voice, threads, partial, models, engines, cache, progress,
                                  audio if streaming else None)
        info = _wave_info(partial)
        partial.replace(output)
    finally:
        partial.unlink(missing_ok=True)
    elapsed = time.perf_counter()-started
    return {"type": "result", "ok": True, "engine": engine, "voice": voice,
            "audio_path": str(output), "output_path": str(output), "threads": threads,
            "wall_seconds": elapsed, "realtime_factor": elapsed/info["duration_seconds"],
            "realtime_multiple": info["duration_seconds"]/elapsed,
            "timing_scope": "cold adapter invocation, including imports and model loading",
            **info, **metadata}


def _cancel(signum, frame):
    if _ACTIVE_NATIVE is not None and _ACTIVE_NATIVE.poll() is None:
        _ACTIVE_NATIVE.terminate()
    raise KeyboardInterrupt("Generation cancelled")


def main() -> int:
    # Node/Electron sends UTF-8 JSON even on Windows installations whose
    # legacy console code page is not UTF-8.
    for channel in (sys.stdin, sys.stdout, sys.stderr):
        if hasattr(channel, "reconfigure"):
            channel.reconfigure(encoding="utf-8", errors="strict")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", type=Path, help="Read request JSON from this file instead of stdin")
    parser.add_argument("--catalog", action="store_true", help="Print supported engine and voice IDs")
    args = parser.parse_args()
    stream = sys.stdout

    def emit(data):
        stream.write(json.dumps(data, ensure_ascii=False, allow_nan=False) + "\n")
        stream.flush()

    if args.catalog:
        emit({"engines": {name: {"default_voice": DEFAULT_VOICES[name], "voices": voices} for name, voices in [("pocket", POCKET_VOICES), ("kokoro", KOKORO_VOICES)]}})
        return 0
    signal.signal(signal.SIGTERM, _cancel)
    signal.signal(signal.SIGINT, _cancel)
    logging.basicConfig(level=logging.WARNING, stream=sys.stderr)
    try:
        request = json.loads(args.request.read_text(encoding="utf-8") if args.request else sys.stdin.read())
        # Keep library messages off the machine-readable JSONL channel.
        with contextlib.redirect_stdout(sys.stderr):
            result = generate(engine=request["engine"], text=request["text"], voice=request.get("voice"),
                              threads=request.get("threads", 4), output_path=request["output_path"],
                              model_root=request["model_root"], engine_root=request.get("engine_root"),
                              cache_root=request.get("cache_root"), progress_callback=emit,
                              streaming=request.get("stream") is True)
        emit(result)
        return 0
    except KeyboardInterrupt:
        emit({"type": "error", "ok": False, "code": "cancelled", "message": "Generation cancelled"})
        return 130
    except Exception as error:
        emit({"type": "error", "ok": False, "code": type(error).__name__, "message": str(error)})
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
