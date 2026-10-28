from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from memory import (
    TECHNIQUE_CATALOG,
    default_memory,
    find_locked_technique,
    find_landmark,
    find_landmark_in_scene,
    forget_landmark,
    landmark_prompt_state,
    landmark_summary,
    learned_technique_ids,
    load_memory,
    memory_summary,
    parse_landmark_command,
    remember_landmark,
    rename_landmark,
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

    def test_landmark_teaching_commands_are_parsed(self) -> None:
        self.assertEqual(
            parse_landmark_command("Remember this is The Ramp"),
            {"operation": "remember", "name": "The Ramp"},
        )
        self.assertEqual(
            parse_landmark_command("Rename The Ramp to Wooden Ramp"),
            {"operation": "rename", "name": "The Ramp", "new_name": "Wooden Ramp"},
        )
        self.assertEqual(
            parse_landmark_command("Update Wooden Ramp to here"),
            {"operation": "update", "name": "Wooden Ramp"},
        )
        self.assertEqual(
            parse_landmark_command("What places do you remember?"),
            {"operation": "list", "name": ""},
        )

    def test_landmark_can_be_saved_resolved_renamed_and_forgotten(self) -> None:
        memory = default_memory()
        state = {
            "level": 9,
            "area": 1,
            "room": 0,
            "facing_yaw": 123,
            "position": {"x": -820, "y": 740, "z": 1250},
        }
        remember_landmark(memory, "The Ramp", state)
        self.assertEqual(find_landmark(memory, "the ramp")["position"]["x"], -820.0)
        self.assertIsNotNone(find_landmark_in_scene(memory, "The Ramp", state))
        renamed = rename_landmark(memory, "The Ramp", "Wooden Ramp")
        self.assertEqual(renamed["name"], "Wooden Ramp")
        forgotten = forget_landmark(memory, "Wooden Ramp")
        self.assertEqual(forgotten["name"], "Wooden Ramp")
        self.assertEqual(landmark_summary(memory), "Mario remembers no landmarks.")

    def test_landmark_scene_and_prompt_distance(self) -> None:
        memory = default_memory()
        saved_state = {
            "level": 9,
            "area": 1,
            "room": 2,
            "position": {"x": 300, "y": 100, "z": 400},
        }
        current_state = {
            "level": 9,
            "area": 1,
            "room": 2,
            "position": {"x": 0, "y": 50, "z": 0},
        }
        remember_landmark(memory, "Hill", saved_state)
        prompt = landmark_prompt_state(memory, current_state)[0]
        self.assertTrue(prompt["scene_matches"])
        self.assertEqual(prompt["horizontal_distance"], 500.0)
        self.assertEqual(prompt["height_delta"], 50.0)
        self.assertIsNone(
            find_landmark_in_scene(
                memory, "Hill", current_state | {"room": 3}
            )
        )


if __name__ == "__main__":
    unittest.main()
