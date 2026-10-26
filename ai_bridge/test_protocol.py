from __future__ import annotations

import unittest
from dataclasses import dataclass, field

from protocol import build_plan_packet, build_status_packet, encode_buttons


@dataclass
class Step:
    type: str
    frames: int
    stick_x: int = 0
    stick_y: int = 0
    buttons: list[str] = field(default_factory=list)
    text: str = ""
    target_id: int = 0
    target_mode: str = "none"
    speed: int = 0
    stop_distance: int = 0


class ProtocolTests(unittest.TestCase):
    def test_button_masks_are_combined(self) -> None:
        self.assertEqual(encode_buttons(["A", "Z"]), 0xA000)

    def test_plan_packet(self) -> None:
        packet = build_plan_packet(
            [
                Step("input", 45, stick_y=80),
                Step("input", 4, stick_y=80, buttons=["A"]),
                Step("wait", 10),
            ]
        )
        self.assertEqual(
            packet,
            "SM64AI1\nINPUT 0 80 0 45\nINPUT 0 80 32768 4\nWAIT 10\nEND\n",
        )

    def test_wait_must_be_neutral(self) -> None:
        with self.assertRaises(ValueError):
            build_plan_packet([Step("wait", 10, stick_y=80)])

    def test_plan_can_lock_to_snapshot_camera_yaw(self) -> None:
        packet = build_plan_packet(
            [Step("input", 10, stick_x=80)], reference_camera_yaw=-12345
        )
        self.assertEqual(
            packet,
            "SM64AI1\nREFERENCE_YAW -12345\nINPUT 80 0 0 10\nEND\n",
        )

    def test_say_becomes_a_neutral_timed_step(self) -> None:
        packet = build_plan_packet([Step("say", 45, text="Let's-a go!")])
        self.assertEqual(
            packet,
            "SM64AI1\nSAY 45 4C657427732D6120676F21\nEND\n",
        )

    def test_say_must_be_first(self) -> None:
        with self.assertRaises(ValueError):
            build_plan_packet([Step("wait", 1), Step("say", 45, text="Hello")])

    def test_target_step_uses_live_object_tracking(self) -> None:
        packet = build_plan_packet(
            [
                Step(
                    "target",
                    180,
                    target_id=17,
                    target_mode="toward",
                    speed=70,
                    stop_distance=150,
                )
            ]
        )
        self.assertEqual(packet, "SM64AI1\nTARGET 17 1 70 0 180 150\nEND\n")

    def test_total_duration_is_limited(self) -> None:
        with self.assertRaises(ValueError):
            build_plan_packet([Step("wait", 300) for _ in range(4)])

    def test_status_packet(self) -> None:
        self.assertEqual(
            build_status_packet("THINKING"),
            "SM64AI1\nSTATUS THINKING\nEND\n",
        )


if __name__ == "__main__":
    unittest.main()
