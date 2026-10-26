from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from runtime import STATE_FILE_NAME, discover_runtime_directory


class RuntimeDiscoveryTests(unittest.TestCase):
    def test_live_build_directory_is_selected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            build = root / "build" / "us_pc"
            build.mkdir(parents=True)
            (build / STATE_FILE_NAME).write_text("{}", encoding="utf-8")

            self.assertEqual(discover_runtime_directory(root), build)

    def test_newest_state_file_wins(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            build = root / "build" / "us_pc"
            build.mkdir(parents=True)
            root_state = root / STATE_FILE_NAME
            build_state = build / STATE_FILE_NAME
            root_state.write_text("{}", encoding="utf-8")
            build_state.write_text("{}", encoding="utf-8")
            os.utime(root_state, ns=(1, 1))
            os.utime(build_state, ns=(2, 2))

            self.assertEqual(discover_runtime_directory(root), build)

    def test_environment_override_wins(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            override = root / "custom"
            with patch.dict(os.environ, {"SM64_AI_RUNTIME_DIR": str(override)}):
                self.assertEqual(discover_runtime_directory(root), override.resolve())


if __name__ == "__main__":
    unittest.main()
