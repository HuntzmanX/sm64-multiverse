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

    def test_announcements_use_their_voices_directions_and_persistent_cache(self):
        for kind, dialog, voice, cue in (
            ("lakitu", 34, "8bc0ef3b96424e6db3cccf6360c69778", "reporter"),
            ("bowser", 21, "4dab911a84414c41ae0438befe7f2e3c", "menacing"),
        ):
            with self.subTest(kind=kind):
                request = self.request | dict(kind=kind, dialog_id=dialog)
                path = self.root / ".sm64-npc-native.json"
                path.write_text(json.dumps(request))
                self.assertEqual(read_native_request(path), request)
                with patch("native_dialogue.synthesize", return_value=b"ID3announcement") as synth:
                    self.assertEqual(self.cache.load_or_generate(request, self.config, lambda: True, self.log), b"ID3announcement")
                    self.assertEqual(synth.call_args.args[1], voice)
                    self.assertIn(cue, synth.call_args.kwargs["script"])
                    DefaultDialogueCache(self.cache.root).load_or_generate(request, self.config, lambda: True, self.log)
                    synth.assert_called_once()
                cached, _ = self.cache.identity(request, self.config)
                changed = replace(self.config, **{f"{kind}_voice": "different-voice"})
                self.assertNotEqual(cached, self.cache.identity(request, changed)[0])
                with self.assertRaises(ValueError):
                    self.cache.identity(request | dict(source="ai"), self.config)

    def test_muted_announcement_does_not_request_or_cache_audio(self):
        for kind in ("lakitu", "bowser"):
            with self.subTest(kind=kind), patch("native_dialogue.synthesize") as synth:
                config = replace(self.config, **{f"{kind}_voice": ""})
                self.assertIsNone(self.cache.load_or_generate(self.request | dict(kind=kind), config, lambda: True))
                synth.assert_not_called()
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

    def test_transient_page_read_failures_do_not_cancel_playback(self):
        path = self.root / ".sm64-npc-native.json"
        path.write_text(json.dumps(self.request))
        with ThreadPoolExecutor(max_workers=1) as pool:
            service = NativeVoiceService(self.root, self.config, pool, self.log)
            for error in (FileNotFoundError, PermissionError, ValueError):
                with self.subTest(error=error), \
                     patch("native_dialogue.read_native_request", side_effect=[error("refresh"), self.request]) as read, \
                     patch("native_dialogue.time.sleep") as sleep:
                    self.assertTrue(service.current(path, self.request["id"]))
                    self.assertEqual(read.call_count, 2)
                    sleep.assert_called_once_with(0.01)
            service.close()

    def test_persistent_loss_and_real_page_changes_still_cancel(self):
        path = self.root / ".sm64-npc-native.json"
        path.write_text(json.dumps(self.request))
        with ThreadPoolExecutor(max_workers=1) as pool:
            service = NativeVoiceService(self.root, self.config, pool, self.log)
            with patch("native_dialogue.read_native_request", side_effect=PermissionError) as read, \
                 patch("native_dialogue.time.sleep") as sleep:
                self.assertFalse(service.current(path, self.request["id"]))
                self.assertEqual(read.call_count, 3)
                self.assertEqual(sleep.call_count, 2)
            with patch("native_dialogue.read_native_request", return_value=self.request | dict(id="1-2-4")) as read, \
                 patch("native_dialogue.time.sleep") as sleep:
                self.assertFalse(service.current(path, self.request["id"]))
                read.assert_called_once()
                sleep.assert_not_called()
            def open_chat(seconds):
                (self.root / ".sm64-npc-request.json").write_text("{}")
            with patch("native_dialogue.read_native_request", side_effect=PermissionError) as read, \
                 patch("native_dialogue.time.sleep", side_effect=open_chat):
                self.assertFalse(service.current(path, self.request["id"]))
                read.assert_called_once()  # Cancellation during retry is immediate.
            service.close()

    def test_cached_clip_survives_repeated_refresh_read_gaps_and_finishes(self):
        path = self.root / ".sm64-npc-native.json"
        path.write_text(json.dumps(self.request))
        cached, _ = self.cache.identity(self.request, self.config)
        cached.parent.mkdir()
        cached.write_bytes(b"ID3cached-recording")
        with ThreadPoolExecutor(max_workers=1) as pool:
            service = NativeVoiceService(self.root, self.config, pool, self.log)
            def play(audio, active, volume):
                self.assertEqual(audio, b"ID3cached-recording")
                for _ in range(20):
                    with patch("native_dialogue.read_native_request", side_effect=[FileNotFoundError, self.request]), \
                         patch("native_dialogue.time.sleep"):
                        self.assertTrue(active())
                return True
            with patch("native_dialogue.synthesize") as synth, patch("native_dialogue.play_audio", side_effect=play) as player:
                service.poll(self.root)
                service._future.result(2)
                player.assert_called_once()
                synth.assert_not_called()
            self.assertEqual((self.root / ".sm64-npc-native-done").read_text(), self.request["id"] + "\n")
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

    def test_finished_page_signals_only_after_playback_and_chains_to_next(self):
        path = self.root / ".sm64-npc-native.json"
        done = self.root / ".sm64-npc-native-done"
        for cached in (False, True):
            with self.subTest(cached=cached), ThreadPoolExecutor(max_workers=1) as pool:
                service = NativeVoiceService(self.root, self.config, pool, self.log)
                pages = [self.request, self.request | dict(id="1-2-4", page=12, text="Next page!")]
                with patch("native_dialogue.synthesize", return_value=b"ID3recording") as synth:
                    for page in pages:
                        done.unlink(missing_ok=True)
                        path.write_text(json.dumps(page))
                        def play(audio, active, volume):
                            self.assertTrue(active())
                            self.assertFalse(done.exists())
                            return True
                        with patch("native_dialogue.play_audio", side_effect=play) as player:
                            service.poll(self.root)
                            service._future.result(2)
                            self.assertEqual(done.read_text(), page["id"] + "\n")
                            service.poll(self.root)
                            player.assert_called_once()  # No repeat playback at a waiting choice.
                    self.assertEqual(synth.call_count, 0 if cached else 2)
                service.close()

    def test_skips_failures_timeouts_and_muted_audio_never_signal_completion(self):
        path = self.root / ".sm64-npc-native.json"
        done = self.root / ".sm64-npc-native-done"
        for scenario in ("skipped", "new-page", "ai-chat", "cancelled", "failed", "muted"):
            with self.subTest(scenario=scenario), ThreadPoolExecutor(max_workers=1) as pool:
                path.write_text(json.dumps(self.request))
                (self.root / ".sm64-npc-request.json").unlink(missing_ok=True)
                config = replace(self.config, bobomb_voice="") if scenario == "muted" else self.config
                service = NativeVoiceService(self.root, config, pool, self.log)
                def play(audio, active, volume):
                    if scenario == "skipped":
                        path.unlink()
                    elif scenario == "new-page":
                        path.write_text(json.dumps(self.request | dict(id="1-2-4", page=12)))
                    elif scenario == "ai-chat":
                        (self.root / ".sm64-npc-request.json").write_text("{}")
                    elif scenario == "failed":
                        raise RuntimeError("device failed")
                    return scenario != "cancelled"  # Includes a player timeout/cancellation result.
                with patch("native_dialogue.synthesize", return_value=b"ID3recording"), \
                     patch("native_dialogue.play_audio", side_effect=play) as player:
                    service.poll(self.root)
                    service._future.result(2)
                    self.assertFalse(done.exists())
                    if scenario == "muted":
                        player.assert_not_called()
                service.close()


if __name__ == "__main__":
    unittest.main()
