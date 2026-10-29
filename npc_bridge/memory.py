"""Keep NPCs separate, and record actions only after the game acknowledges them."""
from __future__ import annotations

import json
from pathlib import Path

from protocol import atomic_write


def load_memory(path: Path) -> dict:
    if not path.exists():
        return {"npcs": {}, "pending": {}, "handled": []}
    memory = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(memory, dict) or not isinstance(memory.get("npcs"), dict):
        raise ValueError("Invalid NPC memory; move it aside to start fresh")
    memory.setdefault("pending", {})
    memory.setdefault("handled", [])
    return memory


def save_memory(path: Path, memory: dict) -> None:
    atomic_write(path, json.dumps(memory, ensure_ascii=True, indent=2) + "\n")


def npc_memory(memory: dict, key: str) -> dict:
    return memory["npcs"].setdefault(key, {"history": [], "coins_given": 0})


def acknowledge(memory: dict, receipt: dict) -> bool:
    pending = memory["pending"].get(receipt.get("id"))
    if not pending or receipt.get("npc") != pending["npc"]:
        return False
    if receipt.get("action") != pending["action"]:
        return False
    coins = receipt.get("coins")
    if type(coins) is not int or not 0 <= coins <= pending["amount"]:
        return False
    if not isinstance(receipt.get("result"), str):
        return False
    entry = npc_memory(memory, pending["npc"])
    entry["coins_given"] += coins
    entry["history"].append({
        "player": pending["player"], "reply": pending["reply"],
        "action": pending["action"], "actual_result": receipt["result"], "coins": coins,
    })
    entry["history"] = entry["history"][-16:]
    del memory["pending"][receipt["id"]]
    return True
