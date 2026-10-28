from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from memory import (
    TECHNIQUE_CATALOG,
    default_memory,
    find_locked_technique,
    learned_technique_ids,
    load_memory,
    memory_summary,
    save_memory,
    technique_prompt_state,
    unlock_seen_techniques,
)


class MemoryTests(unittest.TestCase):
    def test_missing_file_loads_default_memory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            memory = load_memory(Path(directory) / "memory.json")
        self.assertEqual(memory["version"], 1)
        self.assertEqual(learned_technique_ids(memory), set())

    def test_memory_round_trip_and_backup_recovery(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "memory.json"
            memory = default_memory()
            unlock_seen_techniques(memory, {"techniques_seen": ["backflip"]})
            save_memory(path, memory)
            save_memory(path, memory)
            path.write_text("not json", encoding="utf-8")
            recovered = load_memory(path)
        self.assertEqual(learned_technique_ids(recovered), {"backflip"})

    def test_invalid_memory_is_not_saved(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                save_memory(Path(directory) / "memory.json", {"version": 99})

    def test_named_locked_move_is_detected_but_raw_inputs_are_allowed(self) -> None:
        memory = default_memory()
        self.assertEqual(find_locked_technique("Please do a back flip", memory), "backflip")
        self.assertIsNone(
            find_locked_technique("Hold crouch, then jump while crouching", memory)
        )

    def test_learned_move_is_no_longer_locked(self) -> None:
        memory = default_memory()
        unlock_seen_techniques(memory, {"techniques_seen": ["long_jump"]})
        self.assertIsNone(find_locked_technique("Do a long jump", memory))

    def test_only_catalogued_techniques_unlock(self) -> None:
        memory = default_memory()
        unlocked = unlock_seen_techniques(
            memory, {"techniques_seen": ["dive", "made_up_move"]}
        )
        self.assertEqual(unlocked, ["dive"])
        self.assertEqual(learned_technique_ids(memory), {"dive"})

    def test_prompt_state_contains_recipe_only_after_unlock(self) -> None:
        memory = default_memory()
        self.assertEqual(technique_prompt_state(memory), [])
        unlock_seen_techniques(memory, {"techniques_seen": ["side_flip"]})
        prompt = technique_prompt_state(memory)
        self.assertEqual(prompt[0]["name"], "Side Flip")
        self.assertIn("reverse", prompt[0]["recipe"])

    def test_summary_uses_display_names(self) -> None:
        memory = default_memory()
        unlock_seen_techniques(memory, {"techniques_seen": ["ground_pound"]})
        self.assertEqual(memory_summary(memory), "Mario knows: Ground Pound.")

    def test_catalog_has_required_initial_moves(self) -> None:
        self.assertEqual(
            set(TECHNIQUE_CATALOG),
            {"backflip", "side_flip", "long_jump", "slide_jump", "ground_pound", "dive"},
        )


if __name__ == "__main__":
    unittest.main()
