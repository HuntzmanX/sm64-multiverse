import io
import json
import os
import unittest
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest.mock import Mock, patch

from speech import (MAX_AUDIO_BYTES, TOAD_VOICE, MARIO_VOICE, SpeechConfig,
                    _play_windows, _play_audio_unlocked, synthesize, tts_request, prepare_voice, deliver_voice, validate_spoken_reply, play_audio)


class SpeechTests(unittest.TestCase):
    def setUp(self):
        self.config = SpeechConfig("test-key")

    def test_opt_in_and_new_character_voices(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertIsNone(SpeechConfig.from_environment())
        with patch.dict(os.environ, {"FISH_AUDIO_API_KEY": "test-key"}, clear=True):
            config = SpeechConfig.from_environment()
            self.assertEqual(config.model, "drama-3")
            self.assertEqual(config.voice_for("toad"), "e273b9cb9eaf4c5caefb83f6fefa7ac3")
            self.assertEqual(config.voice_for("mario"), "a564085babeb4d14802ea31b7b6c2869")
            self.assertEqual(config.voice_for("bobomb_buddy"), "c4adfd6bd1fe4e59ae18ce19dbce2c14")
            self.assertEqual(config.voice_for("lakitu"), "8bc0ef3b96424e6db3cccf6360c69778")
            self.assertEqual(config.voice_for("bowser"), "4dab911a84414c41ae0438befe7f2e3c")
            self.assertNotIn("test-key", repr(config))

    def test_overrides_and_independent_voice_disable(self):
        with patch.dict(os.environ, {"FISH_AUDIO_API_KEY": "test-key", "FISH_AUDIO_MARIO_VOICE": "",
                                   "FISH_AUDIO_TOAD_VOICE": "alternate"}, clear=True):
            config = SpeechConfig.from_environment()
            self.assertEqual(config.voice_for("mario"), "")
            self.assertEqual(config.voice_for("toad"), "alternate")

    def test_announcement_voice_overrides_and_disable_are_independent(self):
        with patch.dict(os.environ, {"FISH_AUDIO_API_KEY": "test-key", "FISH_AUDIO_LAKITU_VOICE": " alternate ",
                                   "FISH_AUDIO_BOWSER_VOICE": ""}, clear=True):
            config = SpeechConfig.from_environment()
            self.assertEqual(config.voice_for("lakitu"), "alternate")
            self.assertEqual(config.voice_for("bowser"), "")
            self.assertEqual(config.voice_for("toad"), TOAD_VOICE)

    def test_config_rejects_model_typos_and_bad_volume(self):
        for change in [{"FISH_AUDIO_MODEL": "typo"}, {"SM64_NPC_VOICE_VOLUME": "2"}]:
            with patch.dict(os.environ, {"FISH_AUDIO_API_KEY": "test-key", **change}, clear=True):
                with self.assertRaises(ValueError):
                    SpeechConfig.from_environment()

    def test_exact_fish_payload_and_expression(self):
        request = tts_request(self.config, TOAD_VOICE, "Hello Mario!", "excited")
        self.assertEqual(request.full_url, "https://api.fish.audio/v1/tts")
        self.assertEqual(request.get_method(), "POST")
        self.assertEqual(request.get_header("Authorization"), "Bearer test-key")
        self.assertEqual(request.get_header("Model"), "drama-3")
        self.assertEqual(json.loads(request.data), dict(text="[excited] Hello Mario!", reference_id=TOAD_VOICE, format="mp3"))
        neutral = tts_request(self.config, MARIO_VOICE, "Hello!", "neutral")
        self.assertEqual(json.loads(neutral.data), dict(text="Hello!", reference_id=MARIO_VOICE, format="mp3"))

    def test_drama_script_keeps_inline_delivery_cues(self):
        text = "You want coins? Fine."
        script = "[angry, incredulous] You want coins? [whispering] Fine."
        result = tts_request(self.config, TOAD_VOICE, text, "angry", script=script)
        self.assertEqual(json.loads(result.data)["text"], script)
        for invalid in ["[angry] No coins.", "[x[y]] You want coins? Fine.",
                        "<|speaker:1|>You want coins? Fine.", "["+"x"*90+"] You want coins? Fine."]:
            with self.assertRaises(ValueError):
                validate_spoken_reply(text, invalid)

    def test_native_pages_have_explicit_larger_text_limit(self):
        text = "A"*300
        with self.assertRaises(ValueError):
            tts_request(self.config, TOAD_VOICE, text, "neutral")
        request = tts_request(self.config, TOAD_VOICE, text, "neutral", max_characters=1024)
        self.assertEqual(len(json.loads(request.data)["text"]), 300)

    def test_incomplete_audio_is_not_accepted(self):
        response = self.response(b"ID3short")
        response.headers["Content-Length"] = "100"
        with patch("speech.urllib.request.urlopen", return_value=response), self.assertRaises(ValueError):
            synthesize(self.config, TOAD_VOICE, "Hi!", "neutral", lambda: True)

    def test_invalid_emotions_and_long_speech_rejected(self):
        for text, emotion in [("hi", "explode"), ("", "happy"), ("x"*201, "neutral")]:
            with self.assertRaises(ValueError):
                tts_request(self.config, TOAD_VOICE, text, emotion)

    @staticmethod
    def response(audio=b"ID3fake-mp3", content_type="audio/mpeg"):
        response = io.BytesIO(audio)
        response.headers = {"Content-Type": content_type}
        return response

    def test_download_and_no_network_after_cancellation(self):
        with patch("speech.urllib.request.urlopen", return_value=self.response()) as call:
            self.assertEqual(synthesize(self.config, TOAD_VOICE, "Hi!", "happy", lambda: True), b"ID3fake-mp3")
            self.assertEqual(call.call_args.kwargs["timeout"], 20)
        with patch("speech.urllib.request.urlopen") as call:
            self.assertIsNone(synthesize(self.config, TOAD_VOICE, "Hi!", "happy", lambda: False))
            call.assert_not_called()

    def test_bad_content_empty_and_excess_audio_rejected(self):
        for response in [self.response(b"{}", "application/json"), self.response(b""),
                         self.response(b"x" * (MAX_AUDIO_BYTES+1))]:
            with patch("speech.urllib.request.urlopen", return_value=response):
                with self.assertRaises(ValueError):
                    synthesize(self.config, TOAD_VOICE, "Hi!", "neutral", lambda: True)

    def test_reveal_only_on_audio_start_and_exactly_once(self):
        events = []
        def play(audio, active, volume, on_start):
            events.append("audio-start")
            on_start()
            events.append("audio-end")
        with patch("speech.play_audio", side_effect=play):
            deliver_voice(b"audio", self.config, lambda: True, lambda: events.append("text"))
        self.assertEqual(events, ["audio-start", "text", "audio-end"])

    def test_playback_failure_reveals_text_without_repeating_it(self):
        reveal = Mock()
        with patch("speech.play_audio", side_effect=RuntimeError), patch("builtins.print"):
            deliver_voice(b"audio", self.config, lambda: True, reveal, log=Mock())
        reveal.assert_called_once()
        reveal.reset_mock()
        def fail_after_start(audio, active, volume, on_start):
            on_start()
            raise RuntimeError
        with patch("speech.play_audio", side_effect=fail_after_start), patch("builtins.print"):
            deliver_voice(b"audio", self.config, lambda: True, reveal, log=Mock())
        reveal.assert_called_once()

    def test_cancelled_voice_neither_plays_nor_reveals(self):
        with patch("speech.play_audio") as play:
            reveal = Mock()
            deliver_voice(b"audio", self.config, lambda: False, reveal)
            reveal.assert_not_called()
            play.assert_not_called()

    def test_voice_failure_hides_exception_details(self):
        log = Mock()
        with patch("speech.synthesize", side_effect=RuntimeError("test-key")):
            self.assertIsNone(prepare_voice(self.config, "toad", "Hi!", "happy", lambda: True, log))
        self.assertIn("using text", log.call_args.args[0])
        self.assertNotIn("test-key", log.call_args.args[0])
        with patch("speech.synthesize") as synth:
            self.assertIsNone(prepare_voice(None, "mario", "Hi!", "neutral", lambda: True))
            self.assertIsNone(prepare_voice(SpeechConfig("test", bobomb_voice=""), "bobomb_buddy", "Hi!", "neutral", lambda: True))
            synth.assert_not_called()

    def test_native_and_ai_playback_share_one_device(self):
        began, release = threading.Event(), threading.Event()
        counts = [0, 0]
        def play(audio, active, volume, on_start):
            counts[0] += 1
            counts[1] = max(counts[1], counts[0])
            began.set()
            release.wait(2)
            counts[0] -= 1
        with patch("speech._play_audio_unlocked", side_effect=play), ThreadPoolExecutor(max_workers=2) as pool:
            one = pool.submit(play_audio, b"native", lambda: True, 0.8)
            self.assertTrue(began.wait(2))
            two = pool.submit(play_audio, b"ai", lambda: True, 0.8)
            release.set()
            one.result(2); two.result(2)
        self.assertEqual(counts, [0, 1])

    def test_windows_audio_start_callback_and_device_cleanup(self):
        commands = []
        def send(command, buffer, length, callback):
            commands.append(command)
            if command.startswith("status"):
                buffer.value = "stopped"
            return 0
        winmm = Mock()
        winmm.mciSendStringW.side_effect = send
        with patch("speech.ctypes.WinDLL", return_value=winmm, create=True):
            self.assertTrue(_play_windows(Path("voice.mp3"), lambda: True, 0.5, lambda: commands.append("reveal")))
        self.assertTrue(commands[0].startswith('open "'))
        self.assertIn("setaudio sm64npcvoice volume to 500", commands)
        self.assertLess(commands.index("play sm64npcvoice"), commands.index("reveal"))
        self.assertEqual(commands[-1], "close sm64npcvoice")
        commands.clear()
        def fail(command, *args):
            commands.append(command)
            return 1 if command.startswith("play") else 0
        winmm.mciSendStringW.side_effect = fail
        with patch("speech.ctypes.WinDLL", return_value=winmm, create=True), self.assertRaises(RuntimeError):
            _play_windows(Path("voice.mp3"), lambda: True, 1, lambda: commands.append("reveal"))
        self.assertNotIn("reveal", commands)
        self.assertEqual(commands[-1], "close sm64npcvoice")

    def test_windows_cancel_and_timeout_are_not_successful_completion(self):
        commands = []
        def send(command, buffer, length, callback):
            commands.append(command)
            if command.startswith("status"):
                buffer.value = "playing"
            return 0
        winmm = Mock()
        winmm.mciSendStringW.side_effect = send
        with patch("speech.ctypes.WinDLL", return_value=winmm, create=True):
            active = Mock(side_effect=[True, False])
            self.assertFalse(_play_windows(Path("voice.mp3"), active, 0.5))
            self.assertEqual(commands[-1], "close sm64npcvoice")
            with patch("speech.time.monotonic", side_effect=[0, 61]):
                self.assertFalse(_play_windows(Path("voice.mp3"), lambda: True, 0.5))
            self.assertEqual(commands[-1], "close sm64npcvoice")

    def test_ffplay_completion_timeout_and_failure(self):
        for status in (0, None, 1):
            with self.subTest(status=status):
                process = Mock()
                process.poll.return_value = status
                with patch("speech.sys.platform", "linux"), patch("speech.shutil.which", return_value="ffplay"), \
                     patch("speech.subprocess.Popen", return_value=process), \
                     patch("speech.time.monotonic", side_effect=[0, 61]):
                    if status == 1:
                        with self.assertRaises(RuntimeError):
                            _play_audio_unlocked(b"ID3audio", lambda: True, 0.5)
                    else:
                        self.assertEqual(_play_audio_unlocked(b"ID3audio", lambda: True, 0.5), status == 0)
                if status is None:
                    process.terminate.assert_called_once()
                else:
                    process.terminate.assert_not_called()


if __name__ == "__main__":
    unittest.main()
