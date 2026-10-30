"""Validated, ASCII-only file protocol shared with the game. No SDK dependency."""
from __future__ import annotations

import json
import os
import re
import stat
import unicodedata
from pathlib import Path

ACTIONS = ("none", "follow", "flee", "approach", "stop", "hostile", "friendly", "give_coins", "open_cannon")
TOKEN = re.compile(r"^[0-9]+-[0-9]+-[0-9]+$")
NPC_KEY = re.compile(r"^[0-9:.-]+$")


def atomic_write(path: Path, text: str) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(text, encoding="utf-8")
    os.replace(temporary, path)


def read_json(path: Path) -> dict:
    if path.stat().st_size > 128_000:
        raise ValueError("Runtime file is too large")
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError("Expected a JSON object")
    return value


def request_from_file(path: Path) -> dict:
    value = read_json(path)
    if not isinstance(value.get("id"), str) or not TOKEN.fullmatch(value["id"]):
        raise ValueError("Invalid request ID")
    if not isinstance(value.get("npc"), str) or len(value["npc"]) > 127 or not NPC_KEY.fullmatch(value["npc"]):
        raise ValueError("Invalid NPC identity")
    if value.get("kind") not in {"toad", "bobomb_buddy"}:
        raise ValueError("Unsupported NPC")
    for field in ("save", "level", "area", "course", "act", "stars", "dialog_id", "coin_budget"):
        if type(value.get(field)) is not int:
            raise ValueError(f"Invalid {field}")
    if not 0 <= value["coin_budget"] <= 10:
        raise ValueError("Invalid coin budget")
    if type(value.get("can_open_cannon")) is not bool or value.get("mode") not in ACTIONS:
        raise ValueError("Invalid NPC capabilities")
    if not isinstance(value.get("player_text"), str) or not 1 <= len(value["player_text"].strip()) <= 120:
        raise ValueError("Player text must be 1-120 characters")
    if type(value.get("protocol_version", 1)) is not int or value.get("protocol_version", 1) not in (1, 2, 3):
        raise ValueError("Unsupported conversation protocol")
    count = value.get("history_count", 0)
    if type(count) is not int or not 0 <= count <= 32:
        raise ValueError("Invalid history count")
    return value


def ascii_text(text: str, limit: int = 200) -> str:
    text = text.translate(str.maketrans({"’": "'", "‘": "'", "“": '"', "”": '"', "—": "-", "–": "-", "…": "..."}))
    text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode()
    return " ".join(text.split())[:limit]


def response_packet(token: str, reply: str, action: str = "none", amount: int = 0,
                    *, end_conversation: bool = False, version: int = 2) -> str:
    if not TOKEN.fullmatch(token) or action not in ACTIONS:
        raise ValueError("Invalid response identity or action")
    if type(amount) is not int or not 0 <= amount <= 3 or (action != "give_coins" and amount != 0):
        raise ValueError("Invalid reward amount")
    if type(end_conversation) is not bool or type(version) is not int or version not in (1, 2, 3):
        raise ValueError("Invalid conversation completion")
    text = ascii_text(reply)
    if not text:
        raise ValueError("Empty reply")
    ending = f" {int(end_conversation or action != 'none')}" if version >= 3 else ""
    return f"{token}\n{ACTIONS.index(action)} {amount}{ending}\n{text}\n"


def allowed_actions(request: dict) -> list[str]:
    actions = [a for a in ACTIONS if a != "open_cannon"
               and (a != "hostile" or request["kind"] == "bobomb_buddy" or request.get("protocol_version", 1) >= 3)]
    if request["kind"] == "bobomb_buddy" and request["can_open_cannon"]:
        actions.append("open_cannon")
    return actions


def validate_action(request: dict, action: str, amount: int, remembered_coins: int) -> None:
    if action not in allowed_actions(request):
        raise ValueError("This NPC cannot perform that action")
    if type(amount) is not int or not 0 <= amount <= 3:
        raise ValueError("Invalid coin amount")
    if action == "give_coins":
        if not 1 <= amount <= min(request["coin_budget"], max(0, 10 - remembered_coins)):
            raise ValueError("Reward exceeds this NPC's remaining coins")
    elif amount:
        raise ValueError("Only give_coins may specify coins")


def discover_runtime(root: Path) -> Path:
    override = os.environ.get("SM64_NPC_RUNTIME_DIR")
    if override:
        return Path(override).expanduser().resolve()
    candidates = [root / ".sm64-npc-request.json", root / ".sm64-npc-native.json"]
    candidates.extend((root / "build").glob("*/.sm64-npc-request.json"))
    candidates.extend((root / "build").glob("*/.sm64-npc-native.json"))
    latest, latest_time = root, None
    for path in candidates:
        # The game removes/replaces these files on page changes and chat close.
        # Snapshot each candidate once; an existence check followed by another
        # stat can race with that removal (especially on Windows).
        try:
            info = path.stat()
        except OSError:
            continue
        if stat.S_ISREG(info.st_mode) and (latest_time is None or info.st_mtime_ns > latest_time):
            latest, latest_time = path.parent, info.st_mtime_ns
    return latest


def event_packet(token: str, sequence: int, speaker: int, text: str) -> str:
    """Version 2 reveal events: Mario=1, NPC=2, historical Mario/NPC=3/4."""
    if not TOKEN.fullmatch(token) or type(sequence) is not int or sequence < 1:
        raise ValueError("Invalid reveal event identity")
    if type(speaker) is not int or speaker not in (1, 2, 3, 4):
        raise ValueError("Invalid reveal speaker")
    text = ascii_text(text)
    if not text:
        raise ValueError("Empty reveal event")
    return f"{token}\n{sequence} {speaker}\n{text}\n"
