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

    def test_say_becomes_a_neutral_timed_step(self) -> None:
        packet = build_plan_packet([Step("say", 45, text="Let's-a go!")])
        self.assertEqual(packet, "SM64AI1\nWAIT 45\nEND\n")

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
