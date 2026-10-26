from __future__ import annotations

import os
from pathlib import Path


STATE_FILE_NAME = ".sm64-ai-state.json"
COMMAND_FILE_NAME = ".sm64-ai-command"


def _newest_parent(paths: list[Path]) -> Path | None:
    existing = [path for path in paths if path.is_file()]
    if not existing:
        return None
    return max(existing, key=lambda path: path.stat().st_mtime_ns).parent


def discover_runtime_directory(root: Path) -> Path:
    override = os.environ.get("SM64_AI_RUNTIME_DIR")
    if override:
        return Path(override).expanduser().resolve()

    build_root = root / "build"
    state_files = [root / STATE_FILE_NAME]
    config_files = [root / "sm64config.txt"]

    if build_root.is_dir():
        state_files.extend(build_root.glob(f"*/{STATE_FILE_NAME}"))
        config_files.extend(build_root.glob("*/sm64config.txt"))

    runtime_directory = _newest_parent(state_files)
    if runtime_directory is not None:
        return runtime_directory

    runtime_directory = _newest_parent(config_files)
    if runtime_directory is not None:
        return runtime_directory

    return root
