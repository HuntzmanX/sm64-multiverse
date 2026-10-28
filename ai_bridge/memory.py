from __future__ import annotations

import json
import os
import re
import shutil
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


MEMORY_VERSION = 1
PROFILE_NAME = "mario"

TECHNIQUE_CATALOG: dict[str, dict[str, Any]] = {
    "backflip": {
        "name": "Backflip",
        "aliases": ["backflip", "back flip"],
        "requirements": ["grounded", "stationary or moving slowly"],
        "recipe": (
            "From the ground, hold Z for 3-5 frames. Keep Z held and hold A for "
            "8-12 frames, then release both."
        ),
        "locked_reply": "Back-a-what? Show me with the controls!",
        "learned_reply": "Oho! So that's-a the backflip!",
    },
    "side_flip": {
        "name": "Side Flip",
        "aliases": ["side flip", "sideflip"],
        "requirements": ["grounded", "running", "rapid direction reversal"],
        "recipe": (
            "Run left or right for 12-20 frames, reverse that direction and hold A "
            "for 8-12 frames, then release A while keeping the reversed direction."
        ),
        "locked_reply": "A side-a flip? Show me how first!",
        "learned_reply": "Yahoo! Mario learned the side flip!",
    },
    "long_jump": {
        "name": "Long Jump",
        "aliases": ["long jump", "longjump"],
        "requirements": ["grounded", "running forward with enough speed"],
        "recipe": (
            "Build forward speed for 15-30 frames, hold forward and Z for 2-4 "
            "frames, then keep both held and add A for 8-12 frames. Release Z and "
            "A while continuing forward."
        ),
        "locked_reply": "Long jump? You'll have to teach-a me!",
        "learned_reply": "Yahoo! That's-a the long jump!",
    },
    "slide_jump": {
        "name": "Slide Jump",
        "aliases": ["slide jump", "slidejump"],
        "requirements": ["currently butt-sliding"],
        "recipe": (
            "While butt-sliding, hold A for 6-10 frames with the intended stick "
            "direction, then release A and keep steering through the air."
        ),
        "locked_reply": "Jump from a slide? Show me the timing!",
        "learned_reply": "Wahoo! Mario learned the slide jump!",
    },
    "ground_pound": {
        "name": "Ground Pound",
        "aliases": ["ground pound", "ground-pound", "groundpound"],
        "requirements": ["airborne"],
        "recipe": (
            "Jump with A, release A, then press Z for 3-5 frames while airborne "
            "and release it."
        ),
        "locked_reply": "Ground-a pound? Show me what you mean!",
        "learned_reply": "Bam! Mario learned the ground pound!",
    },
    "dive": {
        "name": "Dive",
        "aliases": ["dive", "diving"],
        "requirements": ["moving forward", "airborne or moving quickly"],
        "recipe": (
            "Gain forward motion, jump if needed, then press B for 2-4 frames while "
            "airborne and release B while continuing forward."
        ),
        "locked_reply": "A dive? Show me how it's-a done!",
        "learned_reply": "Let's-a go! Mario learned to dive!",
    },
}


def default_memory() -> dict[str, Any]:
    return {
        "version": MEMORY_VERSION,
        "profiles": {PROFILE_NAME: {"techniques": {}, "landmarks": {}, "routes": []}},
    }


def _valid_memory(value: Any) -> bool:
    if not isinstance(value, dict) or value.get("version") != MEMORY_VERSION:
        return False
    profiles = value.get("profiles")
    if not isinstance(profiles, dict):
        return False
    profile = profiles.get(PROFILE_NAME)
    return (
        isinstance(profile, dict)
        and isinstance(profile.get("techniques"), dict)
        and isinstance(profile.get("landmarks"), dict)
        and isinstance(profile.get("routes"), list)
    )


def _read_memory_file(path: Path) -> dict[str, Any] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return None
    return value if _valid_memory(value) else None


def load_memory(path: Path) -> dict[str, Any]:
    memory = _read_memory_file(path)
    if memory is not None:
        return memory
    backup = _read_memory_file(path.with_name(path.name + ".bak"))
    return backup if backup is not None else default_memory()


def save_memory(path: Path, memory: dict[str, Any]) -> None:
    if not _valid_memory(memory):
        raise ValueError("Refusing to save invalid Mario memory data")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    backup = path.with_name(path.name + ".bak")
    temporary.write_text(
        json.dumps(memory, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n"
    )
    if path.is_file() and _read_memory_file(path) is not None:
        shutil.copy2(path, backup)
    os.replace(temporary, path)


def technique_memory(memory: dict[str, Any]) -> dict[str, Any]:
    return memory["profiles"][PROFILE_NAME]["techniques"]


def learned_technique_ids(memory: dict[str, Any]) -> set[str]:
    return {
        technique_id
        for technique_id, value in technique_memory(memory).items()
        if technique_id in TECHNIQUE_CATALOG
        and isinstance(value, dict)
        and value.get("learned") is True
    }


def technique_prompt_state(memory: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        {
            "id": technique_id,
            "name": TECHNIQUE_CATALOG[technique_id]["name"],
            "requirements": TECHNIQUE_CATALOG[technique_id]["requirements"],
            "recipe": TECHNIQUE_CATALOG[technique_id]["recipe"],
        }
        for technique_id in TECHNIQUE_CATALOG
        if technique_id in learned_technique_ids(memory)
    ]


def find_locked_technique(command: str, memory: dict[str, Any]) -> str | None:
    learned = learned_technique_ids(memory)
    lowered = command.casefold()
    matches: list[tuple[int, int, str]] = []
    for technique_id, definition in TECHNIQUE_CATALOG.items():
        if technique_id in learned:
            continue
        for alias in definition["aliases"]:
            match = re.search(rf"(?<!\w){re.escape(alias.casefold())}(?!\w)", lowered)
            if match is not None:
                matches.append((match.start(), -len(alias), technique_id))
    return min(matches)[2] if matches else None


def unlock_seen_techniques(memory: dict[str, Any], state: dict) -> list[str]:
    seen = state.get("techniques_seen")
    if not isinstance(seen, list):
        return []
    techniques = technique_memory(memory)
    unlocked: list[str] = []
    for technique_id in TECHNIQUE_CATALOG:
        if technique_id not in seen or technique_id in learned_technique_ids(memory):
            continue
        techniques[technique_id] = {
            "learned": True,
            "learned_at": datetime.now(timezone.utc).isoformat(),
        }
        unlocked.append(technique_id)
    return unlocked


def memory_summary(memory: dict[str, Any]) -> str:
    learned = learned_technique_ids(memory)
    if not learned:
        return "Mario has not learned any advanced techniques yet."
    names = [
        TECHNIQUE_CATALOG[technique_id]["name"]
        for technique_id in TECHNIQUE_CATALOG
        if technique_id in learned
    ]
    return "Mario knows: " + ", ".join(names) + "."
