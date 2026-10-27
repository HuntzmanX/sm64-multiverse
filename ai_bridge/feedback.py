from __future__ import annotations

import math
import re
import time
from pathlib import Path
from typing import Any


TERMINAL_STATUSES = {"COMPLETE", "ERROR", "CANCELLED"}


def read_status(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8").strip().upper()
    except OSError:
        return "UNKNOWN"


def status_mtime(path: Path) -> int | None:
    try:
        return path.stat().st_mtime_ns
    except OSError:
        return None


def wait_for_completion(
    path: Path,
    previous_mtime: int | None,
    timeout_seconds: float,
    poll_seconds: float = 0.05,
) -> str:
    deadline = time.monotonic() + timeout_seconds

    while time.monotonic() < deadline:
        current_mtime = status_mtime(path)
        if current_mtime is not None and current_mtime != previous_mtime:
            status = read_status(path)
            if status in TERMINAL_STATUSES:
                return status
        time.sleep(poll_seconds)

    return "TIMEOUT"


def wait_for_fresh_state(
    path: Path,
    after_mtime: int | None,
    timeout_seconds: float = 1.0,
    poll_seconds: float = 0.05,
) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        current_mtime = status_mtime(path)
        if current_mtime is not None and (
            after_mtime is None or current_mtime > after_mtime
        ):
            return
        time.sleep(poll_seconds)


def new_door_transition(before: dict, after: dict) -> dict[str, Any] | None:
    before_transition = before.get("door_transition")
    after_transition = after.get("door_transition")
    if not isinstance(after_transition, dict):
        return None

    before_sequence = (
        before_transition.get("sequence", 0)
        if isinstance(before_transition, dict)
        else 0
    )
    after_sequence = after_transition.get("sequence", 0)
    if (
        not isinstance(before_sequence, int)
        or not isinstance(after_sequence, int)
        or after_sequence <= before_sequence
    ):
        return None
    return after_transition


def remember_door_transition(memory: dict, transition: dict[str, Any]) -> None:
    completed = memory.setdefault("completed_interactions", [])
    if not isinstance(completed, list):
        completed = []
        memory["completed_interactions"] = completed

    sequence = transition.get("sequence")
    if any(
        isinstance(item, dict)
        and item.get("type") == "door_crossed"
        and item.get("sequence") == sequence
        for item in completed
    ):
        return

    completed.append(
        {
            "type": "door_crossed",
            "sequence": sequence,
            "source_id": transition.get("source_id", 0),
            "destination_id": transition.get("destination_id", 0),
            "from": transition.get("from", {}),
            "to": transition.get("to", {}),
        }
    )


def completed_door_ids_for_scene(memory: dict, state: dict) -> set[int]:
    completed = memory.get("completed_interactions")
    if not isinstance(completed, list):
        return set()

    current_scene = (state.get("level"), state.get("area"))
    blocked: set[int] = set()
    for item in completed:
        if not isinstance(item, dict) or item.get("type") != "door_crossed":
            continue
        destination = item.get("to")
        if not isinstance(destination, dict):
            continue
        destination_scene = (destination.get("level"), destination.get("area"))
        if destination_scene != current_scene:
            continue
        destination_id = item.get("destination_id")
        if isinstance(destination_id, int) and destination_id > 0:
            blocked.add(destination_id)
    return blocked


def command_allows_door_reuse(command: str) -> bool:
    return re.search(
        r"\b(back|return|again|twice|reverse|come back|go back|leave through)\b",
        command,
        flags=re.IGNORECASE,
    ) is not None


def repeated_door_target_ids(plan: Any, blocked_ids: set[int]) -> set[int]:
    repeated: set[int] = set()
    for step in getattr(plan, "steps", []):
        if (
            getattr(step, "type", None) == "target"
            and getattr(step, "target_mode", None) == "toward"
            and getattr(step, "target_id", 0) in blocked_ids
        ):
            repeated.add(step.target_id)
    return repeated


def summarize_result(before: dict, after: dict) -> str:
    before_position = before.get("position")
    after_position = after.get("position")
    parts: list[str] = []

    if isinstance(before_position, dict) and isinstance(after_position, dict):
        try:
            dx = float(after_position["x"]) - float(before_position["x"])
            dy = float(after_position["y"]) - float(before_position["y"])
            dz = float(after_position["z"]) - float(before_position["z"])
            parts.append(f"moved {math.hypot(dx, dz):.0f} units")
            parts.append(f"height {dy:+.0f}")
        except (KeyError, TypeError, ValueError):
            pass

    before_health = before.get("health")
    after_health = after.get("health")
    if isinstance(before_health, int) and isinstance(after_health, int):
        health_change = after_health - before_health
        parts.append("health unchanged" if health_change == 0 else f"health {health_change:+d}")

    target_result = after.get("target_result")
    if isinstance(target_result, str) and target_result not in {"", "none"}:
        parts.append(f"target {target_result.replace('_', ' ')}")

    door_transition = new_door_transition(before, after)
    if door_transition is not None:
        source_id = door_transition.get("source_id", "?")
        origin = door_transition.get("from")
        destination = door_transition.get("to")
        if isinstance(origin, dict) and isinstance(destination, dict):
            parts.append(
                f"crossed door #{source_id} "
                f"(room {origin.get('room', '?')} -> {destination.get('room', '?')})"
            )
        else:
            parts.append(f"crossed door #{source_id}")

    before_dialog = before.get("dialog")
    after_dialog = after.get("dialog")
    if isinstance(before_dialog, dict) and isinstance(after_dialog, dict):
        was_active = before_dialog.get("active") is True
        is_active = after_dialog.get("active") is True
        if not was_active and is_active:
            parts.append(f"dialog opened (id {after_dialog.get('id', '?')})")
        elif was_active and not is_active:
            parts.append("dialog closed")
        elif was_active and is_active:
            parts.append(f"dialog remains open (id {after_dialog.get('id', '?')})")

    if not parts:
        return "execution completed; telemetry was unavailable"
    return ", ".join(parts)
