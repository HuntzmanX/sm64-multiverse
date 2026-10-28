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


def new_game_events(before: dict, after: dict) -> list[dict[str, Any]]:
    before_sequence = before.get("event_sequence", 0)
    events = after.get("recent_events")
    if not isinstance(before_sequence, int) or not isinstance(events, list):
        return []

    result: list[dict[str, Any]] = []
    for event in events:
        if not isinstance(event, dict):
            continue
        sequence = event.get("sequence")
        if isinstance(sequence, int) and sequence > before_sequence:
            result.append(event)
    return result


def remember_game_events(memory: dict, events: list[dict[str, Any]]) -> None:
    remembered = memory.setdefault("completed_events", [])
    if not isinstance(remembered, list):
        remembered = []
        memory["completed_events"] = remembered
    known_sequences = {
        item.get("sequence") for item in remembered if isinstance(item, dict)
    }

    targets = memory.setdefault("committed_targets", [])
    if not isinstance(targets, list):
        targets = []
        memory["committed_targets"] = targets

    for event in events:
        sequence = event.get("sequence")
        if sequence not in known_sequences:
            remembered.append(event)
            known_sequences.add(sequence)
        object_id = event.get("object_id")
        if not isinstance(object_id, int) or object_id <= 0:
            continue
        for target in targets:
            if (
                isinstance(target, dict)
                and target.get("object_id") == object_id
                and target.get("object_type") == event.get("object_type")
                and target.get("status") != "completed"
                and event.get("type")
                in {
                    "coin_collected",
                    "enemy_defeated",
                    "object_broken",
                    "star_collected",
                    "key_collected",
                    "item_collected",
                    "switch_activated",
                    "shell_mounted",
                }
            ):
                target["status"] = "completed"
                target["completion_event"] = event


def _nearby_object_types(state: dict) -> dict[int, str]:
    nearby = state.get("nearby_objects")
    if not isinstance(nearby, list):
        return {}
    return {
        item["id"]: item["type"]
        for item in nearby
        if isinstance(item, dict)
        and isinstance(item.get("id"), int)
        and isinstance(item.get("type"), str)
    }


def remember_plan_targets(memory: dict, plan: Any, state: dict, cycle: int) -> None:
    targets = memory.setdefault("committed_targets", [])
    if not isinstance(targets, list):
        targets = []
        memory["committed_targets"] = targets
    known_ids = {
        item.get("object_id") for item in targets if isinstance(item, dict)
    }
    object_types = _nearby_object_types(state)

    for step in getattr(plan, "steps", []):
        object_id = getattr(step, "target_id", 0)
        if (
            getattr(step, "type", None) != "target"
            or getattr(step, "target_mode", None) != "toward"
            or object_id in known_ids
        ):
            continue
        object_type = object_types.get(object_id)
        if object_type is None:
            continue
        targets.append(
            {
                "object_id": object_id,
                "object_type": object_type,
                "status": "active",
                "committed_cycle": cycle,
            }
        )
        known_ids.add(object_id)


def command_allows_multiple_of_type(command: str, object_type: str) -> bool:
    words = object_type.replace("_", " ").split()
    noun = words[-1] if words else object_type
    plural = re.escape(noun) + r"(?:s|es)"
    full_name = re.escape(" ".join(words))
    clauses = re.split(
        r"\s*(?:,|;|\b(?:and then|then|and|but|while|otherwise)\b)\s*",
        command,
        flags=re.IGNORECASE,
    )
    for clause in clauses:
        mentions_type = re.search(
            rf"\b(?:{full_name}|{re.escape(noun)}|{plural})\b",
            clause,
            flags=re.IGNORECASE,
        ) is not None
        if object_type.endswith("coin") and re.search(
            r"\bcoins?\b", clause, flags=re.IGNORECASE
        ):
            mentions_type = True
        if not mentions_type:
            continue
        if re.search(
            r"\b(all|every|each|another|more|remaining|several|multiple|until|twice|"
            r"two|three|four|five|six|seven|eight|nine|ten)\b|\b[2-9]\d*\b",
            clause,
            flags=re.IGNORECASE,
        ) or re.search(rf"\b{plural}\b", clause, flags=re.IGNORECASE):
            return True

    # An explicit follow-up such as "then get another" widens the preceding goal.
    if re.search(
        r"\b(?:and then|then)\s+(?:\w+\s+){0,2}another\b",
        command,
        flags=re.IGNORECASE,
    ):
        return True
    return False


def repeated_completed_target_ids(
    plan: Any, state: dict, memory: dict, command: str
) -> set[int]:
    targets = memory.get("committed_targets")
    if not isinstance(targets, list):
        return set()
    completed_types = {
        item.get("object_type")
        for item in targets
        if isinstance(item, dict)
        and item.get("status") == "completed"
        and isinstance(item.get("object_type"), str)
    }
    object_types = _nearby_object_types(state)
    repeated: set[int] = set()
    for step in getattr(plan, "steps", []):
        if (
            getattr(step, "type", None) != "target"
            or getattr(step, "target_mode", None) != "toward"
        ):
            continue
        object_id = getattr(step, "target_id", 0)
        object_type = object_types.get(object_id)
        if (
            object_type in completed_types
            and not command_allows_multiple_of_type(command, object_type)
        ):
            repeated.add(object_id)
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

    events = new_game_events(before, after)
    for event in events:
        event_type = str(event.get("type", "event")).replace("_", " ")
        object_type = str(event.get("object_type", "")).replace("_", " ")
        object_id = event.get("object_id", 0)
        label = f"{event_type} {object_type}".strip()
        if isinstance(object_id, int) and object_id > 0:
            label += f" #{object_id}"
        parts.append(label)

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
