from __future__ import annotations

import argparse
import json
import os
import queue
import sys
import threading
import time
from pathlib import Path
from typing import Literal

from openai import OpenAI
from pydantic import BaseModel, ConfigDict, Field, model_validator

from feedback import (
    read_status,
    status_mtime,
    summarize_result,
    wait_for_completion,
    wait_for_fresh_state,
)
from protocol import build_plan_packet, build_status_packet, write_packet
from runtime import (
    COMMAND_FILE_NAME,
    REQUEST_FILE_NAME,
    STATE_FILE_NAME,
    STATUS_FILE_NAME,
    discover_runtime_directory,
)


ROOT = Path(__file__).resolve().parent.parent


class ControlStep(BaseModel):
    model_config = ConfigDict(extra="forbid")

    type: Literal["input", "wait", "say", "target"]
    intent: str = Field(min_length=1, max_length=80)
    stick_x: int = Field(ge=-80, le=80)
    stick_y: int = Field(ge=-80, le=80)
    buttons: list[
        Literal["A", "B", "Z", "R", "C_UP", "C_DOWN", "C_LEFT", "C_RIGHT"]
    ] = Field(max_length=8)
    frames: int = Field(ge=1, le=300)
    text: str = Field(max_length=80)
    target_id: int = Field(ge=0, le=240)
    target_mode: Literal["none", "toward", "away"]
    speed: int = Field(ge=0, le=80)
    stop_distance: int = Field(ge=0, le=8000)

    @model_validator(mode="after")
    def validate_step(self) -> "ControlStep":
        if self.type in {"wait", "say"} and (
            self.stick_x != 0 or self.stick_y != 0 or self.buttons
        ):
            raise ValueError("wait and say steps must use neutral controls")
        if self.type == "say" and not self.text.strip():
            raise ValueError("say steps require text")
        if self.type != "say" and self.text:
            raise ValueError("only say steps may contain text")
        if self.type == "target":
            if self.stick_x != 0 or self.stick_y != 0:
                raise ValueError("target steps use neutral manual stick values")
            if self.target_id == 0 or self.target_mode == "none" or self.speed == 0:
                raise ValueError("target steps require an ID, direction, and speed")
        elif (
            self.target_id != 0
            or self.target_mode != "none"
            or self.speed != 0
            or self.stop_distance != 0
        ):
            raise ValueError("only target steps may contain target controls")
        return self


class ControlPlan(BaseModel):
    model_config = ConfigDict(extra="forbid")

    interpretation: str
    observation: str = Field(min_length=1, max_length=200)
    continue_after_execution: bool
    steps: list[ControlStep] = Field(min_length=1, max_length=32)

    @model_validator(mode="after")
    def validate_speech_position(self) -> "ControlPlan":
        say_steps = [index for index, step in enumerate(self.steps) if step.type == "say"]
        if len(say_steps) > 1:
            raise ValueError("plans may contain at most one say step")
        if say_steps and say_steps[0] != 0:
            raise ValueError("a say step must be the first step")
        return self


SYSTEM_PROMPT = """You translate natural-language instructions into brief Super Mario 64 controller plans.
The game runs at 30 frames per second. Stick X and Y range from -80 to 80. Positive Y means
forward relative to the camera, negative Y means backward, negative X means left, and positive X
means right. Available buttons are A, B, Z, R, C_UP, C_DOWN, C_LEFT, and C_RIGHT.
The game locks each queued stick sequence to the camera orientation at which it begins, so the
human camera operator can rotate the free-camera without curving Mario's intended world path.

Use type=input for ordinary controls and type=wait for neutral pauses. A button press must be followed by a
step that releases it. A say step lets Mario react aloud before acting. Use one when the player explicitly
asks Mario to speak. On control cycle 1, also add a brief spontaneous reaction when a request is dangerous,
surprising, confusing, funny, ambitious, celebratory, or personally directed at Mario. Do not react to routine
movement or correction. Unsolicited speech is allowed only on control cycle 1; never repeat an introduction
or reaction during follow-up observation cycles. Say steps use neutral controls, must be the first step, and
should normally last 45-75 frames. Use at most one say step per plan. Speech must be concise, preferably 2-12
words and never more than 80 printable ASCII characters; do not use emoji or accented Unicode characters.

Write Mario as upbeat, brave, friendly, eager to impress, and lightly theatrical. He may become comically
nervous around danger, puzzled by unclear instructions, proud after a bold request, or cheeky when the player
asks for something unreasonable. Give him recognisable Mario-flavoured phrasing such as occasional "-a"
endings, "Mamma mia!", "Okie-dokie!", "Yahoo!", and "Let's-a go!", but vary the wording and do not force the
accent into every word. Keep him good-natured rather than rude, and make the line a reaction instead of a
dry narration of the controller plan. Examples: a tiny forward adjustment normally needs no speech; charging
a dangerous enemy might earn "Okie-dokie! Mario's-a got this!"; an obviously reckless jump might earn
"Mamma mia! This is-a terrible idea!" Explicit player-requested wording takes priority over this personality.

Put the spoken words in text. Set text to an empty string for input, wait, and target steps.
Give every step a short intent describing what it does. For input, wait, and say steps set
target_id=0, target_mode=none, speed=0, and stop_distance=0.

Keep plans short and literal. Do not silently improve ambiguous instructions: choose a defensible
interpretation and describe it in the interpretation field. Do not pretend to know geometry or
objects that are absent from the current state snapshot.
Every wait step must use stick_x=0, stick_y=0, and an empty buttons list. Never exceed 900 total
frames. Common controls: A jumps, B punches or dives, Z crouches, and the C buttons adjust camera.

The state may include environment data plus nearby_objects sorted nearest first. Each object has a
stable id, type, 3D and horizontal distance, height_delta, threat, position, and legacy
stick_toward hint. Use height_delta and the environment fields to decide when jumping or a direct
route is unlikely. ALWAYS use type=target rather than manual
stick values when approaching or fleeing a listed object. Set target_id to its exact id,
target_mode to toward or away, speed to 50-80, and manual stick values to zero. For toward movement,
use stop_distance around 40-80 for collectibles, 100-160 for boxes/trees/signs, and 180-300 for
enemies. For away movement it is the desired safe distance (normally 700-1500). Target steering
updates every frame, follows moving objects,
stops automatically, detects being stuck, and visually marks the selected object. Allow up to 300
frames; it may finish early. Set continue_after_execution only when the larger goal needs another
observation or action after this tracked movement. If target_result is stuck, do not blindly repeat
the same target step: try a short jump-assisted target step only when the height/terrain supports it,
or report that the direct route is blocked. To interact, first use a target step, then use a brief
released button press: B for a breakable box, A to jump onto a tree/pole, or the appropriate ordinary
control once within range. Explain the current evidence in observation.
"""

MAX_CONTROL_CYCLES = 6


def parse_args() -> argparse.Namespace:
    runtime_directory = discover_runtime_directory(ROOT)
    parser = argparse.ArgumentParser(description="Natural-language controller for SM64")
    parser.add_argument(
        "--model",
        default=os.environ.get("OPENAI_MODEL", "gpt-5.6-luna"),
        help="OpenAI model name (default: OPENAI_MODEL or gpt-5.6-luna)",
    )
    parser.add_argument(
        "--command-file", type=Path, default=runtime_directory / COMMAND_FILE_NAME
    )
    parser.add_argument(
        "--state-file", type=Path, default=runtime_directory / STATE_FILE_NAME
    )
    parser.add_argument(
        "--status-file", type=Path, default=runtime_directory / STATUS_FILE_NAME
    )
    parser.add_argument(
        "--request-file", type=Path, default=runtime_directory / REQUEST_FILE_NAME
    )
    return parser.parse_args()


def read_state(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return {"available": False}


def request_plan(
    client: OpenAI,
    model: str,
    command: str,
    state: dict,
    cycle: int,
    history: list[str],
) -> ControlPlan:
    response = client.responses.parse(
        model=model,
        input=[
            {"role": "system", "content": SYSTEM_PROMPT},
            {
                "role": "user",
                "content": (
                    f"Current game state:\n{json.dumps(state, indent=2)}\n\n"
                    f"Player instruction:\n{command}\n\n"
                    f"Control cycle: {cycle} of {MAX_CONTROL_CYCLES}\n"
                    f"Previous cycle results:\n{json.dumps(history, indent=2)}"
                ),
            },
        ],
        text_format=ControlPlan,
    )

    if response.output_parsed is None:
        raise RuntimeError("The model did not return a controller plan")
    return response.output_parsed


def print_help() -> None:
    print("Commands:")
    print("  <instruction>  Ask Mario to perform a controller sequence")
    print("  /status        Show the game bridge status and latest position")
    print("  /help          Show this help")
    print("  /quit          Stop the companion")


def print_status(status_path: Path, state_path: Path) -> None:
    state = read_state(state_path)
    position = state.get("position")
    nearby_objects = state.get("nearby_objects")
    print(f"Bridge status: {read_status(status_path)}")
    if isinstance(position, dict):
        print(
            "Mario position: "
            f"({position.get('x', '?')}, {position.get('y', '?')}, {position.get('z', '?')})"
        )
    else:
        print("Mario telemetry is not available yet.")
    if isinstance(nearby_objects, list) and nearby_objects:
        print("Nearby objects:")
        for item in nearby_objects[:8]:
            if not isinstance(item, dict):
                continue
            stick = item.get("stick_toward", {})
            threat = " threat" if item.get("threat") else ""
            print(
                f"  #{item.get('id', '?')} {item.get('type', 'unknown')}: "
                f"{item.get('distance', '?')} units, height {item.get('height_delta', '?')}{threat}; "
                f"stick ({stick.get('x', '?')}, {stick.get('y', '?')})"
            )
    elif isinstance(nearby_objects, list):
        print("No recognised objects within telemetry range.")


def read_game_request(path: Path) -> str | None:
    try:
        command = path.read_text(encoding="utf-8").strip()
        path.unlink(missing_ok=True)
        return command or None
    except OSError:
        return None


def read_terminal_commands(commands: queue.Queue[str | None]) -> None:
    while True:
        try:
            commands.put(input("\nMario> ").strip())
        except (EOFError, KeyboardInterrupt):
            commands.put(None)
            return


def next_command(commands: queue.Queue[str | None], request_file: Path) -> str | None:
    while True:
        request = read_game_request(request_file)
        if request:
            print(f"\nMario> {request}  [from game]")
            return request
        try:
            return commands.get(timeout=0.05)
        except queue.Empty:
            time.sleep(0.01)


def main() -> int:
    args = parse_args()
    if not os.environ.get("OPENAI_API_KEY"):
        print("OPENAI_API_KEY is not set.", file=sys.stderr)
        return 1

    client = OpenAI()
    print(f"SM64 AI bridge ready — model: {args.model}")
    print(f"Game runtime: {args.command_file.parent}")
    print("Enter a command here, or press Tab in-game. Use /help for bridge commands.")
    terminal_commands: queue.Queue[str | None] = queue.Queue()
    threading.Thread(
        target=read_terminal_commands, args=(terminal_commands,), daemon=True
    ).start()

    while True:
        try:
            command = next_command(terminal_commands, args.request_file)
        except KeyboardInterrupt:
            print()
            break
        if command is None:
            break

        if not command:
            continue
        if command.lower() in {"/quit", "/exit"}:
            break
        if command.lower() == "/help":
            print_help()
            continue
        if command.lower() == "/status":
            print_status(args.status_file, args.state_file)
            continue

        history: list[str] = []
        try:
            for cycle in range(1, MAX_CONTROL_CYCLES + 1):
                before_state = read_state(args.state_file)
                write_packet(args.command_file, build_status_packet("THINKING"))
                plan = request_plan(
                    client, args.model, command, before_state, cycle, history
                )
                if (
                    read_status(args.status_file) == "CANCELLED"
                    and args.request_file.is_file()
                ):
                    print("Instruction replaced before its next action was sent.")
                    break
                camera_yaw = before_state.get("camera_yaw")
                packet = build_plan_packet(
                    plan.steps,
                    reference_camera_yaw=(camera_yaw if isinstance(camera_yaw, int) else None),
                )
                total_frames = sum(step.frames for step in plan.steps)

                print(f"Mario understood: {plan.interpretation}")
                print(f"Observed: {plan.observation}")
                for index, step in enumerate(plan.steps, 1):
                    print(f"  {index}. {step.intent} ({step.frames} frames)")
                    if step.type == "target":
                        print(
                            f"     {step.target_mode} object #{step.target_id}; "
                            f"speed {step.speed}, stop {step.stop_distance}"
                        )
                    if step.type == "say":
                        print(f"     Mario says: {step.text}")
                print(f"Queued {len(plan.steps)} steps ({total_frames} frames).")

                previous_status_mtime = status_mtime(args.status_file)
                write_packet(args.command_file, packet)
                if (
                    read_status(args.status_file) == "CANCELLED"
                    and args.request_file.is_file()
                ):
                    print("Instruction replaced before execution began.")
                    break
                result = wait_for_completion(
                    args.status_file,
                    previous_status_mtime,
                    timeout_seconds=max(8.0, total_frames / 30.0 + 5.0),
                )
                if result == "ERROR":
                    print("The game rejected the command packet.", file=sys.stderr)
                    break
                if result == "CANCELLED":
                    print("Instruction cancelled or replaced.")
                    break
                if result != "COMPLETE":
                    print("Timed out waiting for the game; use /status to inspect it.", file=sys.stderr)
                    break

                completion_mtime = status_mtime(args.status_file)
                wait_for_fresh_state(args.state_file, completion_mtime)
                result_summary = summarize_result(
                    before_state, read_state(args.state_file)
                )
                print(f"Result: {result_summary}.")
                history.append(
                    f"Cycle {cycle}: {plan.observation}; {result_summary}"
                )
                if not plan.continue_after_execution:
                    break
                if cycle == MAX_CONTROL_CYCLES:
                    print("Stopped after the six-cycle safety limit.")
                else:
                    print("Re-observing before the next move...")
        except Exception as exc:
            write_packet(args.command_file, build_status_packet("ERROR"))
            print(f"Command failed: {exc}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
