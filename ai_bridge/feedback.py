from __future__ import annotations

import math
import time
from pathlib import Path


TERMINAL_STATUSES = {"COMPLETE", "ERROR"}


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

    if not parts:
        return "execution completed; telemetry was unavailable"
    return ", ".join(parts)
