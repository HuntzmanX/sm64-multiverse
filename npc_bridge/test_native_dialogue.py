import json
import os
import time
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
from pathlib import Path
from unittest.mock import Mock, patch

from native_dialogue import DefaultDialogueCache, NativeVoiceService, read_native_request
from protocol import atomic_write, discover_runtime
from speech import SpeechConfig


def native(**changes):
    return dict(id="1-2-3", source="default", language="us", kind="bobomb_buddy",
                dialog_id=4, page=0, text="Hello Mario!", **changes)


class NativeCacheTests(unittest.TestCase):
    def setUp(self):
        environment = patch.dict(os.environ, {}, clear=True)
        environment.start()
        self.addCleanup(environment.stop)
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.cache = DefaultDialogueCache(self.root / "npc_audio_cache")
        self.config = SpeechConfig("test-key")
        self.request = native()
        self.log = Mock()

    def test_cache_miss_then_hit_across_new_service_instances(self):
        with patch("native_dialogue.synthesize", return_value=b"ID3complete") as synth:
            first = self.cache.load_or_generate(self.request, self.config, lambda: True, self.log)
            second = DefaultDialogueCache(self.cache.root).load_or_generate(
                self.request, self.config, lambda: True, self.log)
            self.assertEqual(first, second)
            synth.assert_called_once()
            self.assertEqual(synth.call_args.kwargs["max_characters"], 1024)
        self.assertEqual(len(list(self.cache.root.glob("*.mp3"))), 1)
        self.assertFalse(list(self.cache.root.glob("*.tmp")))

    def test_voice_model_text_and_direction_invalidate_cache(self):
        path, _ = self.cache.identity(self.request, self.config)
        configs = [replace(self.config, bobomb_voice="new-voice"), replace(self.config, model="s2.1-pro-free")]
        for config in configs:
            self.assertNotEqual(path, self.cache.identity(self.request, config)[0])
        self.assertNotEqual(path, self.cache.identity(self.request | dict(text="Changed."), self.config)[0])
        with patch.object(self.cache, "direction", return_value="whispering"):
            self.assertNotEqual(path, self.cache.identity(self.request, self.config)[0])
        # Request IDs and save slots do not waste a reusable default recording.
        self.assertEqual(path, self.cache.identity(self.request | dict(id="9-9-9", save=2), self.config)[0])

    def test_ai_dialogue_explicitly_rejected(self):
        with self.assertRaises(ValueError):
            self.cache.load_or_generate(self.request | dict(source="ai"), self.config, lambda: True)
        self.assertFalse(self.cache.root.exists())

    def test_failure_and_cancelled_download_never_save_a_file(self):
        for result in (None, b"", b"{error}"):
            with patch("native_dialogue.synthesize", return_value=result):
                if result is not None:
                    with self.assertRaises(ValueError):
                        self.cache.load_or_generate(self.request, self.config, lambda: True, self.log)
                else:
                    self.assertIsNone(self.cache.load_or_generate(self.request, self.config, lambda: True, self.log))
        self.assertFalse(self.cache.root.exists())
        with patch("native_dialogue.synthesize", side_effect=TimeoutError):
            with self.assertRaises(TimeoutError):
                self.cache.load_or_generate(self.request, self.config, lambda: True, self.log)
        self.assertFalse(self.cache.root.exists())

    def test_corrupt_existing_recording_is_regenerated(self):
        path, _ = self.cache.identity(self.request, self.config)
        path.parent.mkdir()
        path.write_bytes(b"corrupt")
        with patch("native_dialogue.synthesize", return_value=b"ID3new") as synth:
            self.assertEqual(self.cache.load_or_generate(self.request, self.config, lambda: True, self.log), b"ID3new")
        synth.assert_called_once()
        self.assertEqual(path.read_bytes(), b"ID3new")

    def test_parallel_requests_generate_one_recording(self):
        started, release = threading.Event(), threading.Event()
        def generate(*args, **kwargs):
            started.set()
            self.assertTrue(release.wait(2))
            return b"ID3single"
        with ThreadPoolExecutor(max_workers=2) as pool, patch("native_dialogue.synthesize", side_effect=generate) as synth:
            one = pool.submit(self.cache.load_or_generate, self.request, self.config, lambda: True, self.log)
            self.assertTrue(started.wait(2))
            two = pool.submit(self.cache.load_or_generate, self.request, self.config, lambda: True, self.log)
            release.set()
            self.assertEqual(one.result(2), two.result(2))
        synth.assert_called_once()

    def test_native_file_validation_and_runtime_discovery(self):
        build = self.root / "build/us_pc"
        build.mkdir(parents=True)
        path = build / ".sm64-npc-native.json"
        path.write_text(json.dumps(self.request))
        self.assertEqual(read_native_request(path), self.request)
        self.assertEqual(discover_runtime(self.root), build)
        for change in [dict(source="ai"), dict(kind="sign"), dict(page=-1), dict(language="jp"), dict(text="")]:
            path.write_text(json.dumps(self.request | change))
            with self.assertRaises(ValueError):
                read_native_request(path)

    def test_skipping_a_page_finishes_cache_but_does_not_play_stale_audio(self):
        path = self.root / ".sm64-npc-native.json"
        path.write_text(json.dumps(self.request))
        with ThreadPoolExecutor(max_workers=1) as pool:
            service = NativeVoiceService(self.root, self.config, pool, self.log)
            def download(*args, **kwargs):
                path.unlink()
                return b"ID3downloaded"
            with patch("native_dialogue.synthesize", side_effect=download), patch("native_dialogue.play_audio") as play:
                service.poll(self.root)
                service._future.result(2)
                play.assert_not_called()
            self.assertEqual(len(list(service.cache.root.glob("*.mp3"))), 1)
            service.close()

    def test_stale_file_from_a_closed_game_cannot_request_audio(self):
        path = self.root / ".sm64-npc-native.json"
        path.write_text(json.dumps(self.request))
        os.utime(path, (time.time()-10, time.time()-10))
        with ThreadPoolExecutor(max_workers=1) as pool:
            service = NativeVoiceService(self.root, self.config, pool, self.log)
            with patch("native_dialogue.synthesize") as synth:
                service.poll(self.root)
                self.assertIsNone(service._future)
                synth.assert_not_called()
            service.close()

    def test_cached_page_plays_and_ai_conversation_stops_it(self):
        path = self.root / ".sm64-npc-native.json"
        path.write_text(json.dumps(self.request))
        with ThreadPoolExecutor(max_workers=1) as pool:
            service = NativeVoiceService(self.root, self.config, pool, self.log)
            def play(audio, active, volume):
                self.assertTrue(active())
                (self.root / ".sm64-npc-request.json").write_text("{}")
                self.assertFalse(active())
            with patch("native_dialogue.synthesize", return_value=b"ID3recording"), patch("native_dialogue.play_audio", side_effect=play) as player:
                service.poll(self.root)
                service._future.result(2)
                service.poll(self.root)
                player.assert_called_once()
            service.close()


if __name__ == "__main__":
    unittest.main()
