from __future__ import annotations

import json
import math
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


def landmark_memory(memory: dict[str, Any]) -> dict[str, Any]:
    return memory["profiles"][PROFILE_NAME]["landmarks"]


def normalize_landmark_name(name: str) -> str:
    cleaned = " ".join(name.strip().strip("\"'.,!?").split())
    if not 1 <= len(cleaned) <= 40 or not any(character.isalnum() for character in cleaned):
        raise ValueError("Landmark names must contain 1-40 useful characters")
    if not cleaned.isascii():
        raise ValueError("Landmark names currently use printable ASCII")
    return cleaned


def landmark_key(name: str) -> str:
    return normalize_landmark_name(name).casefold()


def parse_landmark_command(command: str) -> dict[str, str] | None:
    text = command.strip()
    patterns = [
        (
            "rename",
            r"^rename\s+(.+?)\s+to\s+(.+?)[.!?]*$",
        ),
        (
            "update",
            r"^update\s+(.+?)\s+to\s+(?:here|this place|this spot)[.!?]*$",
        ),
        (
            "forget",
            r"^forget\s+(.+?)[.!?]*$",
        ),
        (
            "remember",
            r"^remember(?:\s+that)?\s+(?:this place|this spot|this|here)\s+"
            r"(?:is|as|called)\s+(.+?)[.!?]*$",
        ),
        (
            "remember",
            r"^call\s+(?:this place|this spot|this|here)\s+(.+?)[.!?]*$",
        ),
    ]
    for operation, pattern in patterns:
        match = re.match(pattern, text, flags=re.IGNORECASE)
        if match is None:
            continue
        try:
            name = normalize_landmark_name(match.group(1))
            result = {"operation": operation, "name": name}
            if operation == "rename":
                result["new_name"] = normalize_landmark_name(match.group(2))
            return result
        except ValueError:
            return None

    if re.match(
        r"^(?:what|which)\s+(?:places|locations|landmarks)\s+do\s+you\s+remember[?!.]*$",
        text,
        flags=re.IGNORECASE,
    ) or re.match(
        r"^(?:list|show)(?:\s+me)?\s+(?:your\s+)?(?:places|locations|landmarks)[?!.]*$",
        text,
        flags=re.IGNORECASE,
    ):
        return {"operation": "list", "name": ""}
    return None


def _state_location(state: dict) -> dict[str, Any]:
    position = state.get("position")
    if not isinstance(position, dict):
        raise ValueError("Mario's position is not available")
    try:
        x = float(position["x"])
        y = float(position["y"])
        z = float(position["z"])
        level = int(state["level"])
        area = int(state["area"])
        room = int(state.get("room", 0))
        facing_yaw = int(state.get("facing_yaw", 0))
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError("Mario's current scene is not available") from exc
    return {
        "level": level,
        "area": area,
        "room": room,
        "position": {"x": x, "y": y, "z": z},
        "facing_yaw": facing_yaw,
    }


def remember_landmark(memory: dict[str, Any], name: str, state: dict) -> dict[str, Any]:
    display_name = normalize_landmark_name(name)
    key = landmark_key(display_name)
    location = _state_location(state)
    existing = landmark_memory(memory).get(key)
    now = datetime.now(timezone.utc).isoformat()
    record = {
        "name": display_name,
        "aliases": existing.get("aliases", []) if isinstance(existing, dict) else [],
        **location,
        "radius": 200,
        "created_at": existing.get("created_at", now) if isinstance(existing, dict) else now,
        "updated_at": now,
    }
    landmark_memory(memory)[key] = record
    return record


def find_landmark(memory: dict[str, Any], name: str) -> dict[str, Any] | None:
    try:
        key = landmark_key(name)
    except ValueError:
        return None
    direct = landmark_memory(memory).get(key)
    if isinstance(direct, dict):
        return direct
    for record in landmark_memory(memory).values():
        if not isinstance(record, dict):
            continue
        aliases = record.get("aliases", [])
        if any(isinstance(alias, str) and alias.casefold() == key for alias in aliases):
            return record
    return None


def landmark_scene_matches(record: dict[str, Any], state: dict) -> bool:
    if record.get("level") != state.get("level") or record.get("area") != state.get("area"):
        return False
    saved_room = record.get("room", 0)
    current_room = state.get("room", 0)
    return saved_room == 0 or current_room == 0 or saved_room == current_room


def find_landmark_in_scene(
    memory: dict[str, Any], name: str, state: dict
) -> dict[str, Any] | None:
    record = find_landmark(memory, name)
    return record if record is not None and landmark_scene_matches(record, state) else None


def rename_landmark(memory: dict[str, Any], name: str, new_name: str) -> dict[str, Any]:
    record = find_landmark(memory, name)
    if record is None:
        raise ValueError(f"Mario does not remember {normalize_landmark_name(name)}")
    old_key = next(
        key for key, value in landmark_memory(memory).items() if value is record
    )
    display_name = normalize_landmark_name(new_name)
    new_key = landmark_key(display_name)
    if new_key != old_key and new_key in landmark_memory(memory):
        raise ValueError(f"Mario already remembers a place called {display_name}")
    del landmark_memory(memory)[old_key]
    record["name"] = display_name
    record["updated_at"] = datetime.now(timezone.utc).isoformat()
    landmark_memory(memory)[new_key] = record
    return record


def forget_landmark(memory: dict[str, Any], name: str) -> dict[str, Any]:
    record = find_landmark(memory, name)
    if record is None:
        raise ValueError(f"Mario does not remember {normalize_landmark_name(name)}")
    key = next(key for key, value in landmark_memory(memory).items() if value is record)
    del landmark_memory(memory)[key]
    return record


def landmark_prompt_state(memory: dict[str, Any], state: dict) -> list[dict[str, Any]]:
    mario_position = state.get("position")
    result: list[dict[str, Any]] = []
    for record in landmark_memory(memory).values():
        if not isinstance(record, dict):
            continue
        item = {
            "name": record.get("name"),
            "level": record.get("level"),
            "area": record.get("area"),
            "room": record.get("room"),
            "position": record.get("position"),
            "radius": record.get("radius", 200),
            "scene_matches": landmark_scene_matches(record, state),
        }
        saved_position = record.get("position")
        if isinstance(mario_position, dict) and isinstance(saved_position, dict):
            try:
                dx = float(saved_position["x"]) - float(mario_position["x"])
                dz = float(saved_position["z"]) - float(mario_position["z"])
                item["horizontal_distance"] = round(math.hypot(dx, dz), 1)
                item["height_delta"] = round(
                    float(saved_position["y"]) - float(mario_position["y"]), 1
                )
            except (KeyError, TypeError, ValueError):
                pass
        result.append(item)
    return result


def landmark_summary(memory: dict[str, Any]) -> str:
    names = [
        record.get("name", "Unnamed")
        for record in landmark_memory(memory).values()
        if isinstance(record, dict)
    ]
    if not names:
        return "Mario remembers no landmarks."
    return "Mario remembers: " + ", ".join(names) + "."


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
