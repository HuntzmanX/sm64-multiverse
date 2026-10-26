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
    text: str
    target_id: int
    target_mode: str
    speed: int
    stop_distance: int


def encode_buttons(buttons: Iterable[str]) -> int:
    mask = 0
    for button in buttons:
        try:
            mask |= BUTTON_MASKS[button]
        except KeyError as exc:
            raise ValueError(f"Unsupported button: {button}") from exc
    return mask


def build_plan_packet(
    steps: Iterable[StepLike], reference_camera_yaw: int | None = None
) -> str:
    lines = ["SM64AI1"]
    if reference_camera_yaw is not None:
        if not -32768 <= reference_camera_yaw <= 32767:
            raise ValueError("Reference camera yaw must fit in a signed 16-bit value")
        lines.append(f"REFERENCE_YAW {reference_camera_yaw}")
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

        if step.type == "target":
            if step.stick_x != 0 or step.stick_y != 0 or step.text:
                raise ValueError("Target steps use live steering and require neutral stick/text")
            if not 1 <= step.target_id <= 240:
                raise ValueError("Target ID must be between 1 and 240")
            if step.target_mode not in {"toward", "away"}:
                raise ValueError("Target mode must be toward or away")
            if not 1 <= step.speed <= 80:
                raise ValueError("Target speed must be between 1 and 80")
            if not 0 <= step.stop_distance <= 8000:
                raise ValueError("Target stop distance must be between 0 and 8000")
            direction = 1 if step.target_mode == "toward" else -1
            lines.append(
                f"TARGET {step.target_id} {direction} {step.speed} "
                f"{encode_buttons(step.buttons)} {step.frames} {step.stop_distance}"
            )
        elif step.type == "say":
            if step.stick_x != 0 or step.stick_y != 0 or step.buttons:
                raise ValueError("Wait and say steps must have neutral controls")
            if step_count != 1:
                raise ValueError("A say step must be the first step in a plan")
            encoded_text = step.text.encode("ascii", errors="replace")
            if not encoded_text or len(encoded_text) > 80:
                raise ValueError("Say text must contain between 1 and 80 ASCII bytes")
            lines.append(f"SAY {step.frames} {encoded_text.hex().upper()}")
        elif step.type == "wait":
            if step.stick_x != 0 or step.stick_y != 0 or step.buttons:
                raise ValueError("Wait and say steps must have neutral controls")
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
