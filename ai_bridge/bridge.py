from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Literal

from openai import OpenAI
from pydantic import BaseModel, ConfigDict, Field

from protocol import build_plan_packet, build_status_packet, write_packet
from runtime import COMMAND_FILE_NAME, STATE_FILE_NAME, discover_runtime_directory


ROOT = Path(__file__).resolve().parent.parent


class ControlStep(BaseModel):
    model_config = ConfigDict(extra="forbid")

    type: Literal["input", "wait"]
    stick_x: int = Field(ge=-80, le=80)
    stick_y: int = Field(ge=-80, le=80)
    buttons: list[
        Literal["A", "B", "Z", "R", "C_UP", "C_DOWN", "C_LEFT", "C_RIGHT"]
    ] = Field(max_length=8)
    frames: int = Field(ge=1, le=300)


class ControlPlan(BaseModel):
    model_config = ConfigDict(extra="forbid")

    interpretation: str
    steps: list[ControlStep] = Field(min_length=1, max_length=32)


SYSTEM_PROMPT = """You translate natural-language instructions into brief Super Mario 64 controller plans.
The game runs at 30 frames per second. Stick X and Y range from -80 to 80. Positive Y means
forward relative to the camera, negative Y means backward, negative X means left, and positive X
means right. Available buttons are A, B, Z, R, C_UP, C_DOWN, C_LEFT, and C_RIGHT.

Use type=input for controls and type=wait for neutral pauses. A button press must be followed by a
step that releases it. Keep plans short and literal. Do not silently improve ambiguous instructions:
choose a defensible interpretation and describe it in the interpretation field. The state snapshot
does not contain nearby objects or geometry yet, so do not pretend to know their positions. Every
wait step must use stick_x=0, stick_y=0, and an empty buttons list. Never exceed 900 total frames.
"""


def parse_args() -> argparse.Namespace:
    runtime_directory = discover_runtime_directory(ROOT)
    parser = argparse.ArgumentParser(description="Natural-language controller for SM64")
    parser.add_argument(
        "--model",
        default=os.environ.get("OPENAI_MODEL", "gpt-6-astra"),
        help="OpenAI model name (default: OPENAI_MODEL or gpt-6-astra)",
    )
    parser.add_argument(
        "--command-file", type=Path, default=runtime_directory / COMMAND_FILE_NAME
    )
    parser.add_argument(
        "--state-file", type=Path, default=runtime_directory / STATE_FILE_NAME
    )
    return parser.parse_args()


def read_state(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return {"available": False}


def request_plan(client: OpenAI, model: str, command: str, state: dict) -> ControlPlan:
    response = client.responses.parse(
        model=model,
        input=[
            {"role": "system", "content": SYSTEM_PROMPT},
            {
                "role": "user",
                "content": (
                    f"Current game state:\n{json.dumps(state, indent=2)}\n\n"
                    f"Player instruction:\n{command}"
                ),
            },
        ],
        text_format=ControlPlan,
    )

    if response.output_parsed is None:
        raise RuntimeError("The model did not return a controller plan")
    return response.output_parsed


def main() -> int:
    args = parse_args()
    if not os.environ.get("OPENAI_API_KEY"):
        print("OPENAI_API_KEY is not set.", file=sys.stderr)
        return 1

    client = OpenAI()
    print(f"SM64 AI bridge ready — model: {args.model}")
    print(f"Game runtime: {args.command_file.parent}")
    print("Enter a command, or /quit to stop.")

    while True:
        try:
            command = input("\nMario> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if not command:
            continue
        if command.lower() in {"/quit", "/exit"}:
            break

        write_packet(args.command_file, build_status_packet("THINKING"))
        try:
            plan = request_plan(client, args.model, command, read_state(args.state_file))
            packet = build_plan_packet(plan.steps)
            print(f"Mario understood: {plan.interpretation}")
            print(f"Queued {len(plan.steps)} steps ({sum(step.frames for step in plan.steps)} frames).")
            write_packet(args.command_file, packet)
        except Exception as exc:
            write_packet(args.command_file, build_status_packet("ERROR"))
            print(f"Command failed: {exc}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
