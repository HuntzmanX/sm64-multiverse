"""Two-speaker turn timing: prepare concurrently, reveal on playback, never overlap."""
from __future__ import annotations

import time
import threading
from concurrent.futures import Executor, TimeoutError
from pathlib import Path

from protocol import ascii_text, atomic_write, event_packet, request_from_file, response_packet
from speech import SpeechConfig, deliver_voice, prepare_voice


def current_request(path: Path, token: str) -> bool:
    try:
        return request_from_file(path)["id"] == token
    except (OSError, ValueError):
        return False


class GameEvents:
    def __init__(self, request: dict, runtime: Path):
        self.token = request["id"]
        self.runtime = runtime
        self.version = request.get("protocol_version", 1)
        self.sequence = 0

    def active(self) -> bool:
        return current_request(self.runtime / ".sm64-npc-request.json", self.token)

    def emit(self, speaker: int, text: str) -> None:
        if self.version < 2 or not self.active():
            return
        self.sequence += 1
        atomic_write(self.runtime / ".sm64-npc-event", event_packet(self.token, self.sequence, speaker, text))
        # Acknowledgement prevents a fast text-only turn from overwriting Mario's entry.
        deadline = time.monotonic() + 2
        while self.active() and time.monotonic() < deadline:
            try:
                ack = (self.runtime / ".sm64-npc-event-ack").read_text().splitlines()
                if ack == [self.token, str(self.sequence)]:
                    return
            except OSError:
                pass
            time.sleep(0.01)
        if self.active():
            raise TimeoutError("Game did not acknowledge the conversation event; rebuild the game")


class ConversationTurn:
    def __init__(self, request: dict, runtime: Path, config: SpeechConfig | None, pool: Executor, log=print):
        self.request, self.runtime, self.config, self.pool, self.log = request, runtime, config, pool, log
        self.events = GameEvents(request, runtime)
        self.player_shown = False
        self.mario_prepared = threading.Event()
        self.preparation_cancelled = threading.Event()

    def reveal_player(self):
        if not self.player_shown and self.events.active():
            self.events.emit(1, self.request["player_text"])
            self.player_shown = True

    def _prepare_npc(self, generate):
        reply = generate()
        text = ascii_text(reply.reply)
        # Prioritize Mario's TTS request, then prepare the NPC audio during his playback.
        while not self.mario_prepared.wait(0.05):
            if not self.events.active() or self.preparation_cancelled.is_set():
                return reply, text, None
        active = lambda: self.events.active() and not self.preparation_cancelled.is_set()
        audio = prepare_voice(self.config, self.request["kind"], text, reply.emotion, active, self.log,
                              script=getattr(reply, "spoken_reply", None))
        return reply, text, audio

    def run(self, generate, entry: dict, remember) -> bool:
        if not self.events.active():
            return False
        # Restore previous acknowledged dialogue on the first turn after entering an area.
        if self.request.get("history_count", 1) == 0:
            for old in entry["history"][-16:]:
                self.events.emit(3, old["player"])
                self.events.emit(4, old["reply"])
        future = self.pool.submit(self._prepare_npc, generate)
        try:
            mario_audio = prepare_voice(self.config, "mario", self.request["player_text"],
                                        "neutral", self.events.active, self.log)
            self.mario_prepared.set()
            deliver_voice(mario_audio, self.config, self.events.active, self.reveal_player, self.log)
            while self.events.active():
                try:
                    reply, text, npc_audio = future.result(timeout=0.1)
                    break
                except TimeoutError:
                    if future.done():
                        raise  # A timeout inside generation, rather than Future.result's wait.
            else:
                return False
            if not self.events.active():
                return False
            remember(reply, text)
            end_conversation = reply.end_conversation or reply.action != "none"
            revealed_at = time.monotonic()
            def reveal_npc():
                nonlocal revealed_at
                self.events.emit(2, text)
                revealed_at = time.monotonic()
            played = deliver_voice(npc_audio, self.config, self.events.active, reveal_npc, self.log)
            if end_conversation and not played:
                # With no voice/device, leave the final line readable before closing.
                deadline = revealed_at + min(8.0, max(1.5, len(text) / 20.0))
                while self.events.active() and time.monotonic() < deadline:
                    time.sleep(0.05)
            if not self.events.active():
                return False
            atomic_write(self.runtime / ".sm64-npc-response",
                         response_packet(self.request["id"], text, reply.action, reply.coins,
                                         end_conversation=end_conversation, version=self.events.version))
            return True
        finally:
            self.preparation_cancelled.set()
            self.mario_prepared.set()
            future.cancel()

    def fail(self) -> None:
        if not self.events.active():
            return
        self.reveal_player()
        text = "Sorry, I lost my train of thought. Could you say that again?"
        self.events.emit(2, text)
        if self.events.active():
            atomic_write(self.runtime / ".sm64-npc-response",
                         response_packet(self.request["id"], text, version=self.events.version))
