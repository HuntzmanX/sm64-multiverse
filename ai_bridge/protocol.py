from __future__ import annotations

import os
from pathlib import Path
from typing import Iterable, Protocol


BUTTON_MASKS = {
    "A": 0x8000,
    "B": 0x4000,
    "Z": 0x2000,
    "R": 0x0010,
    "C_UP": 0x0008,
    "C_DOWN": 0x0004,
    "C_LEFT": 0x0002,
    "C_RIGHT": 0x0001,
}


class StepLike(Protocol):
    type: str
    stick_x: int
    stick_y: int
    buttons: list[str]
    frames: int


def encode_buttons(buttons: Iterable[str]) -> int:
    mask = 0
    for button in buttons:
        try:
            mask |= BUTTON_MASKS[button]
        except KeyError as exc:
            raise ValueError(f"Unsupported button: {button}") from exc
    return mask


def build_plan_packet(steps: Iterable[StepLike]) -> str:
    lines = ["SM64AI1"]
    total_frames = 0
    step_count = 0

    for step in steps:
        step_count += 1
        if step_count > 32:
            raise ValueError("A plan cannot contain more than 32 steps")
        if not 1 <= step.frames <= 300:
            raise ValueError("Every step must last between 1 and 300 frames")

        total_frames += step.frames
        if total_frames > 900:
            raise ValueError("A plan cannot exceed 900 frames")

        if step.type == "wait":
            if step.stick_x != 0 or step.stick_y != 0 or step.buttons:
                raise ValueError("Wait steps must have neutral controls")
            lines.append(f"WAIT {step.frames}")
        elif step.type == "input":
            if not -80 <= step.stick_x <= 80 or not -80 <= step.stick_y <= 80:
                raise ValueError("Stick values must be between -80 and 80")
            lines.append(
                f"INPUT {step.stick_x} {step.stick_y} "
                f"{encode_buttons(step.buttons)} {step.frames}"
            )
        else:
            raise ValueError(f"Unsupported step type: {step.type}")

    if step_count == 0:
        raise ValueError("A plan must contain at least one step")

    lines.append("END")
    return "\n".join(lines) + "\n"


def build_status_packet(status: str) -> str:
    if status not in {"READY", "THINKING", "ERROR"}:
        raise ValueError(f"Unsupported status: {status}")
    return f"SM64AI1\nSTATUS {status}\nEND\n"


def write_packet(path: Path, packet: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = path.with_name(path.name + ".tmp")
    temporary_path.write_text(packet, encoding="utf-8", newline="\n")
    os.replace(temporary_path, path)
