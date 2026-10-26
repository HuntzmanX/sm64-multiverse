from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from feedback import (
    read_status,
    summarize_result,
    wait_for_completion,
    wait_for_fresh_state,
)


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

    def test_wait_for_fresh_state_accepts_an_existing_newer_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "state"
            path.write_text("{}", encoding="utf-8")
            wait_for_fresh_state(path, None, 0.1, 0.001)


if __name__ == "__main__":
    unittest.main()
