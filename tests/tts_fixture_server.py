#!/usr/bin/env python3
"""Local protocol fixture. Synthetic tone only; this is not a speech model.

The server binds loopback on an ephemeral port and prints that port once.
Run with --port 8765 for manual app verification. No network dependencies.
"""
import argparse
import base64
import io
import hashlib
import json
import math
import struct
import time
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs


PCM = b"".join(struct.pack("<h", int(3000 * math.sin(i * 2 * math.pi * 440 / 16000))) for i in range(6400))
stream = io.BytesIO()
with wave.open(stream, "wb") as audio:
    audio.setnchannels(1)
    audio.setsampwidth(2)
    audio.setframerate(16000)
    audio.writeframes(PCM)
WAV = stream.getvalue()
WORDS = [{"word": "Hello", "start": 0.0, "end": 0.15}, {"word": "world", "start": 0.2, "end": 0.38}]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def respond(self, body, content_type="application/json", status=200, **headers):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for name, value in headers.items():
            self.send_header(name.replace("_", "-"), str(value))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def websocket_send(self, event):
        payload = json.dumps(event).encode()
        length = len(payload)
        prefix = bytes([0x81, length]) if length < 126 else (b"\x81\x7e" + struct.pack("!H", length)
                  if length < 65536 else b"\x81\x7f" + struct.pack("!Q", length))
        try:
            self.wfile.write(prefix + payload)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def websocket_read(self):
        prefix = self.rfile.read(2)
        if len(prefix) < 2 or (prefix[0] & 15) == 8:
            return None
        length = prefix[1] & 127
        if length == 126:
            length = struct.unpack("!H", self.rfile.read(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self.rfile.read(8))[0]
        if length > 4 * 1024 * 1024 or not prefix[1] & 128:
            return None
        mask = self.rfile.read(4)
        data = self.rfile.read(length)
        if len(mask) != 4 or len(data) != length:
            return None
        return json.loads(bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def realtime(self):
        self.close_connection = True
        path = urlparse(self.path)
        if (self.headers.get("Authorization") != "Bearer fixture-key"
                or parse_qs(path.query).get("model") != ["gpt-realtime-2.1-mini"]):
            self.respond({"error": "fixture-secret-key"}, status=401)
            return
        if path.path.endswith("redirect"):
            self.respond(b"", status=302, Location="http://example.invalid/capture")
            return
        accept = base64.b64encode(hashlib.sha1((self.headers.get("Sec-WebSocket-Key", "")
                 + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                         "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n").encode())
        self.wfile.flush()
        self.websocket_send({"type": "session.created", "session": {"id": "fixture_session"}})
        update = self.websocket_read() or {}
        session = update.get("session", {})
        valid = (update.get("type") == "session.update"
                 and session.get("model") == "gpt-realtime-2.1-mini"
                 and session.get("tools") == [] and session.get("tool_choice") == "none"
                 and session.get("output_modalities") == ["audio"]
                 and session.get("audio", {}).get("input", {}).get("turn_detection") is None
                 and session.get("audio", {}).get("output", {}).get("format") == {"type": "audio/pcm", "rate": 24000})
        if not valid:
            self.websocket_send({"type": "error", "error": {"message": "Invalid session; fixture-secret-key"}})
            return
        self.websocket_send({"type": "session.updated", "session": session})
        item_event = self.websocket_read() or {}
        item = item_event.get("item", {})
        if (item_event.get("type") != "conversation.item.create" or item.get("role") != "user"
                or item.get("content", [{}])[0].get("type") != "input_text"):
            self.websocket_send({"type": "error", "error": {"message": "Invalid input"}})
            return
        text = item["content"][0]["text"]
        self.websocket_send({"type": "conversation.item.added", "item": item})
        response_event = self.websocket_read() or {}
        if response_event != {"type": "response.create", "response": {"output_modalities": ["audio"]}}:
            self.websocket_send({"type": "error", "error": {"message": "Invalid response request"}})
            return
        if path.path.endswith("delay"):
            time.sleep(1.6)
        self.websocket_send({"type": "response.created", "response": {"id": "reading_1"}})
        shared = {"response_id": "reading_1", "output_index": 0, "content_index": 0}
        if path.path.endswith("oversize"):
            self.websocket_send({"type": "response.output_audio.delta", **shared, "delta": "A" * (4 * 1024 * 1024)})
            return
        if path.path.endswith("error"):
            self.websocket_send({"type": "error", "error": {"message": "fixture-secret-key and private input"}})
            return
        self.websocket_send({"type": "response.output_audio.delta", **shared, "delta": base64.b64encode(PCM).decode()})
        self.websocket_send({"type": "response.output_audio.done", **shared})
        transcript = text
        if path.path.endswith("sign-drift"):
            transcript = text.replace("-5", "5")
        elif path.path.endswith("number-drift"):
            transcript = text.replace("1.2", "12")
        elif path.path.endswith("paraphrase"):
            transcript = "Here is a summary of the message."
        elif path.path.endswith("normalized"):
            transcript = "  " + text.upper().replace(" ", "  ") + "  "
        if not path.path.endswith("no-transcript"):
            self.websocket_send({"type": "response.output_audio_transcript.done", **shared, "transcript": transcript})
        self.websocket_send({"type": "response.done", "response": {"id": "reading_1", "status": "completed"}})

    def do_GET(self):
        if self.headers.get("Upgrade", "").lower() == "websocket":
            self.realtime()
            return
        if self.path == "/openapi.json":
            self.respond({"openapi": "3.1.0", "info": {"title": "vLLM-Omni protocol fixture"},
                "paths": {"/v1/audio/speech": {"post": {"requestBody": {"content": {"application/json": {
                    "schema": {"$ref": "#/components/schemas/SpeechRequest"}}}}}}},
                "components": {"schemas": {"SpeechRequest": {"properties": {
                    "input": {}, "voice": {}, "language": {}, "instructions": {}, "stream": {}, "word_timestamps": {}}}}}})
        elif self.path == "/api/v1/models?output_modalities=speech":
            self.respond({"data": [
                {"id": "fixture/tts", "architecture": {"output_modalities": ["speech"]},
                 "supported_voices": ["fixture-voice", "another-voice"]},
                {"id": "fixture/chat", "architecture": {"output_modalities": ["text"]}},
                {"id": "fixture/audio-chat", "architecture": {"output_modalities": ["audio"]}}]})
        elif self.path == "/v1/models":
            if self.headers.get("xi-api-key") == "fixture-key":
                self.respond([
                    {"model_id": "fixture-tts", "can_do_text_to_speech": True},
                    {"model_id": "fixture-conversion", "can_do_text_to_speech": False}])
            elif self.headers.get("Authorization") == "Bearer fixture-key":
                self.respond({"data": [{"id": "gpt-4o-mini-tts"}, {"id": "gpt-realtime-2.1-mini"}, {"id": "chat-only"}]})
            else:
                self.respond({"data": [{"id": "fixture-speech"}]})
        elif self.path == "/v2/voices":
            if self.headers.get("xi-api-key") != "fixture-key" or self.headers.get("Authorization"):
                self.respond({"error": "incorrect provider authentication"}, status=401)
            else:
                self.respond({"voices": [{"voice_id": "fixture-voice", "name": "Fixture voice"}]})
        elif self.path == "/v1/audio/voices":
            self.respond({"voices": [{"id": "fixture", "name": "Fixture voice"}]})
        elif self.path == "/audio.wav":
            self.respond(WAV, "audio/wav")
        else:
            self.respond({"error": "not found"}, status=404)

    def do_POST(self):
        data = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        try:
            request = json.loads(data)
        except ValueError:
            self.respond({"error": "invalid json"}, status=400)
            return
        path = self.path
        if path == "/api/v1/audio/speech":
            valid = (self.headers.get("Authorization") == "Bearer fixture-key"
                     and not self.headers.get("xi-api-key")
                     and request == {"input": "Hosted speech.", "model": "fixture/tts",
                                     "voice": "fixture-voice", "response_format": "mp3"})
            self.respond(WAV if valid else {"error": "incorrect OpenRouter request"},
                         "audio/wav" if valid else "application/json", status=200 if valid else 400)
        elif path == "/v1/text-to-speech/fixture-voice?output_format=mp3_44100_128":
            valid = (self.headers.get("xi-api-key") == "fixture-key"
                     and not self.headers.get("Authorization")
                     and request == {"text": "Hosted speech.", "model_id": "fixture-tts"})
            self.respond(WAV if valid else {"error": "incorrect ElevenLabs request"},
                         "audio/wav" if valid else "application/json", status=200 if valid else 400)
        elif path in ("/v1/audio/speech", "/binary"):
            self.respond(WAV, "audio/wav")
        elif path == "/json":
            self.respond({"audio": base64.b64encode(WAV).decode(), "format": "wav", "words": WORDS})
        elif path == "/url":
            self.respond({"audio_url": "/audio.wav", "words": WORDS})
        elif path == "/foreign-url":
            self.respond({"audio_url": "http://example.invalid/audio.wav"})
        elif path == "/template":
            expected = 'A "quoted" line\n${voice} 🙂'
            if request.get("text") != expected or request.get("speaker") != "fixture":
                self.respond({"error": "unsafe substitution"}, status=400)
            else:
                self.respond(WAV, "audio/wav")
        elif path == "/sse":
            events = []
            for chunk in (PCM[:6400], PCM[6400:]):
                event = {"type": "speech.audio.delta", "audio": base64.b64encode(chunk).decode(),
                         "response_format": "pcm", "sample_rate": 16000, "channels": 1}
                events.append("event: speech.audio.delta\ndata: " + json.dumps(event) + "\n\n")
            events.append('event: speech.audio.done\ndata: {"type":"speech.audio.done"}\n\n')
            self.respond("".join(events).encode(), "text/event-stream")
        elif path == "/pcm":
            self.respond(PCM, "audio/pcm", X_Audio_Sample_Rate=16000, X_Audio_Channels=1)
        elif path == "/delay":
            time.sleep(1.6)
            self.respond(WAV, "audio/wav")
        elif path == "/redirect":
            self.respond(b"", status=302, Location="http://example.invalid/capture")
        elif path == "/bad-audio":
            self.respond(b"<html>this is not audio</html>", "text/html")
        elif path == "/bad-base64":
            self.respond({"audio": "not!valid!base64"})
        elif path == "/unauthorized":
            self.respond({"error": "invalid token"}, status=401)
        elif path in ("/credit-error", "/json-error"):
            self.respond({"error": "Provider echoed fixture-secret-key and submitted text"},
                         status=402 if path == "/credit-error" else 200)
        else:
            self.respond({"error": "not found"}, status=404)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=0)
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.daemon_threads = True
    print(server.server_port, flush=True)
    server.serve_forever()
