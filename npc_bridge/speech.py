"""Optional Fish Audio speech. One worker; no key, audio or extra deps in git."""
from __future__ import annotations

import ctypes
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

TOAD_VOICE = "91a1e83b577448fbac8ef6fe050f67c9"
EMOTIONS = {"neutral", "excited", "happy", "sad", "angry", "whispering"}
MODELS = {"s1", "s2-pro", "s2.1-pro", "s2.1-pro-free", "drama-3"}
MAX_AUDIO_BYTES = 4 * 1024 * 1024


@dataclass(frozen=True)
class SpeechConfig:
    api_key: str = field(repr=False)
    model: str = "s2.1-pro-free"
    toad_voice: str = TOAD_VOICE
    bobomb_voice: str = ""
    volume: float = 0.8

    @classmethod
    def from_environment(cls) -> SpeechConfig | None:
        key = os.environ.get("FISH_AUDIO_API_KEY", "").strip()
        if not key:
            return None
        model = os.environ.get("FISH_AUDIO_MODEL", "s2.1-pro-free")
        if model not in MODELS:
            # Fish falls back to a paid model for unknown names. Reject typos locally.
            raise ValueError("Unknown FISH_AUDIO_MODEL")
        volume = float(os.environ.get("SM64_NPC_VOICE_VOLUME", "0.8"))
        if not 0 <= volume <= 1:
            raise ValueError("SM64_NPC_VOICE_VOLUME must be between 0 and 1")
        return cls(key, model, os.environ.get("FISH_AUDIO_TOAD_VOICE", TOAD_VOICE).strip(),
                   os.environ.get("FISH_AUDIO_BOBOMB_VOICE", "").strip(), volume)

    def voice_for(self, kind: str) -> str:
        return self.toad_voice if kind == "toad" else self.bobomb_voice if kind == "bobomb_buddy" else ""


def tts_request(config: SpeechConfig, voice: str, text: str, emotion: str) -> urllib.request.Request:
    if emotion not in EMOTIONS:
        raise ValueError("Unsupported speech emotion")
    if not text.strip() or len(text) > 200 or not voice:
        raise ValueError("Speech needs a voice and a short reply")
    spoken = text if emotion == "neutral" else f"[{emotion}] {text}"
    return urllib.request.Request(
        "https://api.fish.audio/v1/tts",
        headers={"Authorization": f"Bearer {config.api_key}", "model": config.model,
                 "Content-Type": "application/json", "Accept": "audio/mpeg"},
        data=json.dumps({"text": spoken, "reference_id": voice, "format": "mp3"}).encode(),
        method="POST",
    )


def synthesize(config: SpeechConfig, voice: str, text: str, emotion: str,
               active: Callable[[], bool]) -> bytes | None:
    if not active():
        return None
    request = tts_request(config, voice, text, emotion)
    with urllib.request.urlopen(request, timeout=20) as response:
        content_type = response.headers.get("Content-Type", "").split(";")[0].lower()
        if content_type and content_type not in {"audio/mpeg", "audio/mp3", "application/octet-stream"}:
            raise ValueError("Fish Audio returned an unexpected content type")
        chunks, size = [], 0
        while active():
            chunk = response.read(64 * 1024)
            if not chunk:
                if not size:
                    raise ValueError("Fish Audio returned empty audio")
                return b"".join(chunks) if active() else None
            size += len(chunk)
            if size > MAX_AUDIO_BYTES:
                raise ValueError("Fish Audio response exceeds audio limit")
            chunks.append(chunk)
    return None


def _play_windows(path: Path, active: Callable[[], bool], volume: float) -> None:
    winmm = ctypes.WinDLL("winmm")
    send = winmm.mciSendStringW
    send.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint, ctypes.c_void_p]
    send.restype = ctypes.c_uint
    alias = "sm64npcvoice"

    def command(text: str) -> str:
        buffer = ctypes.create_unicode_buffer(128)
        error = send(text, buffer, len(buffer), None)
        if error:
            raise RuntimeError(f"Windows audio error {error}")
        return buffer.value

    filename = str(path.resolve())
    if '"' in filename:
        raise ValueError("Unsupported audio filename")
    command(f'open "{filename}" type mpegvideo alias {alias}')
    try:
        command(f"setaudio {alias} volume to {round(volume * 1000)}")
        if not active():
            return
        command(f"play {alias}")
        deadline = time.monotonic() + 60
        while active() and time.monotonic() < deadline:
            if command(f"status {alias} mode").lower() == "stopped":
                break
            time.sleep(0.05)
    finally:
        command(f"close {alias}")


def play_audio(audio: bytes, active: Callable[[], bool], volume: float) -> None:
    if not active():
        return
    with tempfile.TemporaryDirectory(prefix="sm64-npc-voice-") as directory:
        path = Path(directory) / "speech.mp3"
        path.write_bytes(audio)
        if sys.platform == "win32":
            _play_windows(path, active, volume)
            return
        player = shutil.which("ffplay")
        if not player:
            raise RuntimeError("Install ffplay or run the companion with native Windows Python")
        # Arguments are passed directly: never execute model text in a shell.
        process = subprocess.Popen([player, "-nodisp", "-autoexit", "-loglevel", "error",
                                    "-volume", str(round(volume * 100)), str(path)],
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 60
            while process.poll() is None and active() and time.monotonic() < deadline:
                time.sleep(0.05)
            code = process.poll()
            if code not in (None, 0) and active():
                raise RuntimeError(f"Audio player exited with code {code}")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=1)


@dataclass
class _SpeechJob:
    voice: str
    text: str
    emotion: str
    current: Callable[[], bool]
    cancelled: threading.Event = field(default_factory=threading.Event)

    def active(self) -> bool:
        return not self.cancelled.is_set() and self.current()


class SpeechWorker:
    def __init__(self, config: SpeechConfig, log=print):
        self.config, self.log = config, log
        self._queue: queue.Queue = queue.Queue(maxsize=1)
        self._lock = threading.Lock()
        self._active: _SpeechJob | None = None
        self._closed = threading.Event()
        self._thread = threading.Thread(target=self._run, name="npc-speech", daemon=True)
        self._thread.start()

    def cancel(self) -> None:
        with self._lock:
            if self._active:
                self._active.cancelled.set()
            try:
                queued = self._queue.get_nowait()
                queued.cancelled.set()
            except queue.Empty:
                pass

    def speak(self, kind: str, text: str, emotion: str, current: Callable[[], bool]) -> None:
        self.cancel()
        voice = self.config.voice_for(kind)
        if voice and self.config.volume and not self._closed.is_set():
            self._queue.put_nowait(_SpeechJob(voice, text, emotion, current))

    def close(self) -> None:
        self._closed.set()
        self.cancel()
        self._thread.join(timeout=1)

    def _run(self) -> None:
        while not self._closed.is_set():
            # Dequeue and publish the active job under the same lock as cancellation.
            with self._lock:
                try:
                    job = self._queue.get_nowait()
                except queue.Empty:
                    job = None
                self._active = job
            if job is None:
                self._closed.wait(0.05)
                continue
            try:
                if job.active():
                    audio = synthesize(self.config, job.voice, job.text, job.emotion, job.active)
                    if audio is not None and job.active():
                        play_audio(audio, job.active, self.config.volume)
            except Exception as exc:
                if job.active():
                    detail = f"HTTP {exc.code}" if isinstance(exc, urllib.error.HTTPError) else type(exc).__name__
                    self.log(f"Voice unavailable ({detail}); text chat continues.", flush=True)
            finally:
                with self._lock:
                    if self._active is job:
                        self._active = None
