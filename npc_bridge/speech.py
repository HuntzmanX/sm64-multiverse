"""Optional Fish Audio speech. One worker; no key, audio or extra deps in git."""
from __future__ import annotations

import ctypes
import json
import os
import re
import threading
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

TOAD_VOICE = "e273b9cb9eaf4c5caefb83f6fefa7ac3"
MARIO_VOICE = "a564085babeb4d14802ea31b7b6c2869"
BOBOMB_VOICE = "c4adfd6bd1fe4e59ae18ce19dbce2c14"
PLAYBACK_LOCK = threading.Lock()
EMOTIONS = {"neutral", "excited", "happy", "sad", "angry", "whispering"}
MODELS = {"s1", "s2-pro", "s2.1-pro", "s2.1-pro-free", "drama-3"}
MAX_AUDIO_BYTES = 4 * 1024 * 1024


@dataclass(frozen=True)
class SpeechConfig:
    api_key: str = field(repr=False)
    model: str = "drama-3"
    toad_voice: str = TOAD_VOICE
    bobomb_voice: str = BOBOMB_VOICE
    volume: float = 0.8
    mario_voice: str = MARIO_VOICE

    @classmethod
    def from_environment(cls) -> SpeechConfig | None:
        key = os.environ.get("FISH_AUDIO_API_KEY", "").strip()
        if not key:
            return None
        model = os.environ.get("FISH_AUDIO_MODEL", "drama-3")
        if model not in MODELS:
            # Fish falls back to a paid model for unknown names. Reject typos locally.
            raise ValueError("Unknown FISH_AUDIO_MODEL")
        volume = float(os.environ.get("SM64_NPC_VOICE_VOLUME", "0.8"))
        if not 0 <= volume <= 1:
            raise ValueError("SM64_NPC_VOICE_VOLUME must be between 0 and 1")
        return cls(key, model, os.environ.get("FISH_AUDIO_TOAD_VOICE", TOAD_VOICE).strip(),
                   os.environ.get("FISH_AUDIO_BOBOMB_VOICE", BOBOMB_VOICE).strip(), volume,
                   os.environ.get("FISH_AUDIO_MARIO_VOICE", MARIO_VOICE).strip())

    def voice_for(self, kind: str) -> str:
        return {"toad": self.toad_voice, "mario": self.mario_voice,
                "bobomb_buddy": self.bobomb_voice}.get(kind, "")


def validate_spoken_reply(text: str, script: str, max_characters: int = 200) -> str:
    """Permit actor directions, but never different words or executable speaker markup."""
    from protocol import ascii_text
    if not isinstance(script, str) or len(script) > max_characters + 400:
        raise ValueError("Speech script is too long")
    cues = re.findall(r"\[([^\[\]]*)\]", script)
    if len(cues) > 4 or any(not re.fullmatch(r"[A-Za-z][A-Za-z0-9 ,.'!?;:-]{0,79}", cue) for cue in cues):
        raise ValueError("Invalid speech direction")
    plain = re.sub(r"\[[^\[\]]*\]", "", script)
    if any(c in plain for c in "[]<>"):
        raise ValueError("Unsupported speech markup")
    if ascii_text(plain, 2000) != ascii_text(text, 2000):
        raise ValueError("Spoken words must match the displayed reply")
    return " ".join(script.split())


def tts_request(config: SpeechConfig, voice: str, text: str, emotion: str,
                *, script: str | None = None, max_characters: int = 200) -> urllib.request.Request:
    if emotion not in EMOTIONS:
        raise ValueError("Unsupported speech emotion")
    if not text.strip() or len(text) > max_characters or not voice:
        raise ValueError("Speech needs a voice and a bounded text")
    if script is not None:
        spoken = validate_spoken_reply(text, script, max_characters)
    elif emotion == "neutral":
        spoken = text
    else:
        spoken = f"({emotion}) {text}" if config.model == "s1" else f"[{emotion}] {text}"
    return urllib.request.Request(
        "https://api.fish.audio/v1/tts",
        headers={"Authorization": f"Bearer {config.api_key}", "model": config.model,
                 "Content-Type": "application/json", "Accept": "audio/mpeg"},
        data=json.dumps({"text": spoken, "reference_id": voice, "format": "mp3"}).encode(),
        method="POST",
    )


def synthesize(config: SpeechConfig, voice: str, text: str, emotion: str,
               active: Callable[[], bool], *, script: str | None = None,
               max_characters: int = 200) -> bytes | None:
    if not active():
        return None
    request = tts_request(config, voice, text, emotion, script=script, max_characters=max_characters)
    with urllib.request.urlopen(request, timeout=20) as response:
        content_type = response.headers.get("Content-Type", "").split(";")[0].lower()
        if content_type and content_type not in {"audio/mpeg", "audio/mp3", "application/octet-stream"}:
            raise ValueError("Fish Audio returned an unexpected content type")
        length_header = response.headers.get("Content-Length")
        expected = int(length_header) if length_header else None
        if expected is not None and not 0 < expected <= MAX_AUDIO_BYTES:
            raise ValueError("Invalid audio content length")
        chunks, size = [], 0
        while active():
            chunk = response.read(64 * 1024)
            if not chunk:
                if not size:
                    raise ValueError("Fish Audio returned empty audio")
                if expected is not None and size != expected:
                    raise ValueError("Incomplete audio download")
                return b"".join(chunks) if active() else None
            size += len(chunk)
            if size > MAX_AUDIO_BYTES:
                raise ValueError("Fish Audio response exceeds audio limit")
            chunks.append(chunk)
    return None


def _play_windows(path: Path, active: Callable[[], bool], volume: float,
                  on_start: Callable[[], None] = lambda: None) -> None:
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
        on_start()
        deadline = time.monotonic() + 60
        while active() and time.monotonic() < deadline:
            if command(f"status {alias} mode").lower() == "stopped":
                break
            time.sleep(0.05)
    finally:
        command(f"close {alias}")


def _play_audio_unlocked(audio: bytes, active: Callable[[], bool], volume: float,
               on_start: Callable[[], None] = lambda: None) -> None:
    if not active():
        return
    with tempfile.TemporaryDirectory(prefix="sm64-npc-voice-") as directory:
        path = Path(directory) / "speech.mp3"
        path.write_bytes(audio)
        if sys.platform == "win32":
            _play_windows(path, active, volume, on_start)
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
            on_start()
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


def play_audio(audio: bytes, active: Callable[[], bool], volume: float,
               on_start: Callable[[], None] = lambda: None) -> None:
    # Native dialogue and AI conversations share the Windows MCI device/alias.
    while active():
        if PLAYBACK_LOCK.acquire(timeout=0.05):
            try:
                if active():
                    _play_audio_unlocked(audio, active, volume, on_start)
            finally:
                PLAYBACK_LOCK.release()
            return


def prepare_voice(config: SpeechConfig | None, kind: str, text: str, emotion: str,
                  active: Callable[[], bool], log=print, *, script: str | None = None) -> bytes | None:
    if not config or not config.volume or not config.voice_for(kind) or not active():
        return None
    try:
        return synthesize(config, config.voice_for(kind), text, emotion, active, script=script)
    except Exception as exc:
        if active():
            detail = f"HTTP {exc.code}" if isinstance(exc, urllib.error.HTTPError) else type(exc).__name__
            log(f"Voice unavailable ({detail}); using text for this message.", flush=True)
        return None


def deliver_voice(audio: bytes | None, config: SpeechConfig | None, active: Callable[[], bool],
                  on_start: Callable[[], None], log=print) -> bool:
    """Reveal once; return whether audio played successfully (otherwise text fallback)."""
    revealed = False
    played = False

    def reveal_once():
        nonlocal revealed
        if not revealed and active():
            on_start()
            revealed = True

    if not active():
        return False
    try:
        if audio is not None and config:
            play_audio(audio, active, config.volume, reveal_once)
            played = revealed
    except Exception as exc:
        if active():
            log(f"Audio playback unavailable ({type(exc).__name__}); using text.", flush=True)
    finally:
        # No voice, a failed device, or an empty response still leaves a readable turn.
        reveal_once()
    return played
