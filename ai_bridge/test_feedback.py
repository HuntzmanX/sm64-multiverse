from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from feedback import (
    command_allows_door_reuse,
    completed_door_ids_for_scene,
    new_door_transition,
    read_status,
    remember_door_transition,
    repeated_door_target_ids,
    summarize_result,
    wait_for_completion,
    wait_for_fresh_state,
)


class FakeStep:
    def __init__(self, target_id: int) -> None:
        self.type = "target"
        self.target_mode = "toward"
        self.target_id = target_id


class FakePlan:
    def __init__(self, *target_ids: int) -> None:
        self.steps = [FakeStep(target_id) for target_id in target_ids]


class FeedbackTests(unittest.TestCase):
    def test_read_status_normalizes_text(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "status"
            path.write_text("complete\n", encoding="utf-8")
            self.assertEqual(read_status(path), "COMPLETE")

    def test_wait_for_completion_accepts_new_terminal_status(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "status"
            path.write_text("COMPLETE\n", encoding="utf-8")
            self.assertEqual(wait_for_completion(path, None, 0.1, 0.001), "COMPLETE")

    def test_cancelled_is_terminal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "status"
            path.write_text("CANCELLED\n", encoding="utf-8")
            self.assertEqual(wait_for_completion(path, None, 0.1, 0.001), "CANCELLED")

    def test_result_summary_reports_displacement_and_health(self) -> None:
        before = {"position": {"x": 0, "y": 10, "z": 0}, "health": 2176}
        after = {"position": {"x": 3, "y": 12, "z": 4}, "health": 1920}
        self.assertEqual(
            summarize_result(before, after),
            "moved 5 units, height +2, health -256",
        )

    def test_result_summary_reports_target_outcome(self) -> None:
        result = summarize_result(
            {"position": {"x": 0, "y": 0, "z": 0}},
            {
                "position": {"x": 10, "y": 0, "z": 0},
                "target_result": "stuck",
            },
        )
        self.assertIn("target stuck", result)

    def test_result_summary_reports_dialog_transitions(self) -> None:
        opened = summarize_result(
            {"dialog": {"active": False, "id": -1}},
            {"dialog": {"active": True, "id": 3}},
        )
        closed = summarize_result(
            {"dialog": {"active": True, "id": 3}},
            {"dialog": {"active": False, "id": -1}},
        )
        self.assertIn("dialog opened (id 3)", opened)
        self.assertIn("dialog closed", closed)

    def test_new_door_transition_requires_a_new_sequence(self) -> None:
        before = {"door_transition": {"sequence": 3}}
        transition = {
            "sequence": 4,
            "source_id": 17,
            "destination_id": 23,
            "from": {"level": 6, "area": 1, "room": 1},
            "to": {"level": 6, "area": 1, "room": 2},
        }
        self.assertEqual(
            new_door_transition(before, {"door_transition": transition}), transition
        )
        self.assertIsNone(
            new_door_transition(
                {"door_transition": {"sequence": 4}},
                {"door_transition": transition},
            )
        )

    def test_door_transition_is_remembered_and_summarized(self) -> None:
        before = {"door_transition": {"sequence": 0}}
        transition = {
            "sequence": 1,
            "source_id": 17,
            "destination_id": 23,
            "from": {"level": 6, "area": 1, "room": 1},
            "to": {"level": 6, "area": 1, "room": 2},
        }
        after = {"door_transition": transition}
        memory: dict = {"completed_interactions": []}

        remember_door_transition(memory, transition)
        remember_door_transition(memory, transition)

        self.assertEqual(len(memory["completed_interactions"]), 1)
        self.assertIn("crossed door #17 (room 1 -> 2)", summarize_result(before, after))
        current_state = after | {"level": 6, "area": 1}
        self.assertEqual(completed_door_ids_for_scene(memory, current_state), {23})

    def test_loop_guard_only_blocks_completed_destination_door(self) -> None:
        blocked = {23}
        self.assertEqual(repeated_door_target_ids(FakePlan(23, 42), blocked), {23})
        self.assertEqual(repeated_door_target_ids(FakePlan(42), blocked), set())

    def test_explicit_return_instruction_allows_door_reuse(self) -> None:
        self.assertTrue(command_allows_door_reuse("Go through the door and come back"))
        self.assertFalse(command_allows_door_reuse("Enter the door"))

    def test_wait_for_fresh_state_accepts_an_existing_newer_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "state"
            path.write_text("{}", encoding="utf-8")
            wait_for_fresh_state(path, None, 0.1, 0.001)


if __name__ == "__main__":
    unittest.main()
