"""Only original, game-supplied NPC pages are persisted as reusable audio."""
from __future__ import annotations

import hashlib
import json
import os
import tempfile
import threading
import time
from concurrent.futures import Executor
from pathlib import Path

from protocol import TOKEN, ascii_text, read_json
from speech import MAX_AUDIO_BYTES, SpeechConfig, play_audio, synthesize


def read_native_request(path: Path) -> dict:
    request = read_json(path)
    if request.get("source") != "default" or request.get("kind") not in {"toad", "bobomb_buddy"}:
        raise ValueError("Only original NPC pages may enter the voice cache")
    if not isinstance(request.get("id"), str) or not TOKEN.fullmatch(request["id"]):
        raise ValueError("Invalid original dialogue request ID")
    for field in ("dialog_id", "page"):
        if type(request.get(field)) is not int or not 0 <= request[field] < 32768:
            raise ValueError("Invalid original dialogue location")
    if not isinstance(request.get("text"), str) or not 1 <= len(request["text"]) <= 1024:
        raise ValueError("Invalid original dialogue text")
    if request.get("language") != "us":
        raise ValueError("Original dialogue voice decoding currently supports US English")
    return request


def valid_mp3(audio: bytes) -> bool:
    return 0 < len(audio) <= MAX_AUDIO_BYTES and (
        audio.startswith(b"ID3") or len(audio) >= 2 and audio[0] == 255 and audio[1] & 0xE0 == 0xE0)


class DefaultDialogueCache:
    def __init__(self, root: Path):
        self.root = root
        self._locks: dict[str, threading.Lock] = {}
        self._guard = threading.Lock()

    @staticmethod
    def direction(kind: str) -> str:
        return "friendly, animated, a little nervous" if kind == "toad" else "boisterous, cheerful and confident"

    def identity(self, request: dict, config: SpeechConfig) -> tuple[Path, str]:
        if request.get("source") != "default" or request.get("kind") not in {"toad", "bobomb_buddy"}:
            raise ValueError("Only original supported NPC dialogue may be cached")
        text = ascii_text(request["text"], 1024)
        script = f"[{self.direction(request['kind'])}] {text}"
        identity = dict(version=1, model=config.model, voice=config.voice_for(request["kind"]),
                        text=text, script=script, language=request["language"], format="mp3")
        digest = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
        name = f"{request['kind']}_{request['dialog_id']:03d}_{request['page']}_{digest}.mp3"
        return self.root / name, script

    def load_or_generate(self, request: dict, config: SpeechConfig, active, log=print) -> bytes | None:
        path, script = self.identity(request, config)
        if not config.voice_for(request["kind"]) or not config.volume:
            return None
        with self._guard:
            lock = self._locks.setdefault(path.name, threading.Lock())
        while active():
            if not lock.acquire(timeout=0.05):
                continue
            try:
                try:
                    if path.stat().st_size <= MAX_AUDIO_BYTES:
                        audio = path.read_bytes()
                        if valid_mp3(audio):
                            log(f"Original dialogue cache hit: {request['dialog_id']} / {request['page']}", flush=True)
                            return audio
                except OSError:
                    pass
                if not active():
                    return None
                text = ascii_text(request["text"], 1024)
                # Full original pages have a larger text bound than short AI replies.
                audio = synthesize(config, config.voice_for(request["kind"]), text, "neutral", active,
                                   script=script, max_characters=1024)
                if audio is None or not active():
                    return None
                if not valid_mp3(audio):
                    raise ValueError("Original dialogue generation returned invalid MP3")
                self.root.mkdir(parents=True, exist_ok=True)
                descriptor, temporary = tempfile.mkstemp(prefix=".native-", suffix=".tmp", dir=self.root)
                try:
                    with os.fdopen(descriptor, "wb") as file:
                        file.write(audio)
                    os.replace(temporary, path)
                finally:
                    Path(temporary).unlink(missing_ok=True)
                log(f"Cached original dialogue: {request['dialog_id']} / {request['page']}", flush=True)
                return audio
            finally:
                lock.release()
        return None


class NativeVoiceService:
    def __init__(self, root: Path, config: SpeechConfig | None, pool: Executor, log=print):
        self.config, self.pool, self.log = config, pool, log
        directory = os.environ.get("SM64_NPC_AUDIO_CACHE_DIR")
        self.cache = DefaultDialogueCache(Path(directory).expanduser().resolve() if directory else root / "npc_audio_cache")
        self._closed = threading.Event()
        self._future = None
        self._last = None

    def current(self, path: Path, token: str) -> bool:
        try:
            return (not self._closed.is_set() and time.time() - path.stat().st_mtime < 3
                    and read_native_request(path)["id"] == token
                    and not (path.parent / ".sm64-npc-request.json").exists())
        except (OSError, ValueError):
            return False

    def poll(self, runtime: Path) -> None:
        if not self.config or not self.config.volume or self._closed.is_set():
            return
        path = runtime / ".sm64-npc-native.json"
        try:
            request = read_native_request(path)
        except (OSError, ValueError):
            return
        if not self.current(path, request["id"]):
            return
        identity = (path, request["id"])
        if identity == self._last:
            return
        self._last = identity
        if self._future:
            self._future.cancel()  # Queued stale pages need no generation.
        self._future = self.pool.submit(self._run, path, request)

    def _run(self, path: Path, request: dict) -> None:
        current = lambda: self.current(path, request["id"])
        if not current():
            return
        try:
            # Once an encountered page's download starts, finish caching it even if skipped.
            # Only playback follows the live page, so it never talks over a different page.
            audio = self.cache.load_or_generate(request, self.config, lambda: not self._closed.is_set(), self.log)
            if audio is not None and current():
                play_audio(audio, current, self.config.volume)
        except Exception as exc:
            detail = f"HTTP {exc.code}" if hasattr(exc, "code") else type(exc).__name__
            self.log(f"Original dialogue voice unavailable ({detail}); normal dialogue continues.", flush=True)

    def close(self) -> None:
        self._closed.set()
        if self._future:
            self._future.cancel()
