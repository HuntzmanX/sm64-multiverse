from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from protocol import build_plan_packet, write_packet
from runtime import COMMAND_FILE_NAME, discover_runtime_directory


ROOT = Path(__file__).resolve().parent.parent


@dataclass
class Step:
    type: str
    frames: int
    stick_x: int = 0
    stick_y: int = 0
    buttons: list[str] = field(default_factory=list)


def main() -> None:
    runtime_directory = discover_runtime_directory(ROOT)
    packet = build_plan_packet(
        [
            Step("input", 60, stick_y=80),
            Step("input", 4, stick_y=80, buttons=["A"]),
            Step("input", 26, stick_y=80),
            Step("wait", 15),
        ]
    )
    command_file = runtime_directory / COMMAND_FILE_NAME
    write_packet(command_file, packet)
    print(f"Sent the deterministic run-and-jump packet to {command_file}.")


if __name__ == "__main__":
    main()
