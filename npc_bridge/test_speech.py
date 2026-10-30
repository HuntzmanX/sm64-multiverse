import io
import json
import os
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from speech import (MAX_AUDIO_BYTES, TOAD_VOICE, SpeechConfig, SpeechWorker,
                    _play_windows, synthesize, tts_request)


class SpeechTests(unittest.TestCase):
    def setUp(self):
        self.config = SpeechConfig("test-key")

    def test_opt_in_and_default_voice(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertIsNone(SpeechConfig.from_environment())
        with patch.dict(os.environ, {"FISH_AUDIO_API_KEY": "test-key"}, clear=True):
            config = SpeechConfig.from_environment()
            self.assertEqual(config.model, "s2.1-pro-free")
            self.assertEqual(config.voice_for("toad"), TOAD_VOICE)
            self.assertEqual(config.voice_for("bobomb_buddy"), "")
            self.assertNotIn("test-key", repr(config))

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
        self.assertEqual(request.get_header("Model"), "s2.1-pro-free")
        self.assertEqual(json.loads(request.data), dict(text="[excited] Hello Mario!", reference_id=TOAD_VOICE, format="mp3"))
        neutral = tts_request(self.config, TOAD_VOICE, "Hello!", "neutral")
        self.assertEqual(json.loads(neutral.data)["text"], "Hello!")

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

    def test_worker_speaks_on_background_thread(self):
        played = threading.Event()
        threads = []
        def play(*args):
            threads.append(threading.current_thread().name)
            played.set()
        with patch("speech.synthesize", return_value=b"audio"), patch("speech.play_audio", side_effect=play):
            worker = SpeechWorker(self.config)
            try:
                worker.speak("toad", "Hello!", "neutral", lambda: True)
                self.assertTrue(played.wait(2))
                self.assertEqual(threads, ["npc-speech"])
            finally:
                worker.close()

    def test_new_reply_cancels_inflight_audio(self):
        started, release, played = threading.Event(), threading.Event(), threading.Event()
        said = []
        def generate(config, voice, text, emotion, active):
            if text == "old":
                started.set()
                release.wait(2)
            return text.encode() if active() else None
        def play(audio, *args):
            said.append(audio)
            played.set()
        with patch("speech.synthesize", side_effect=generate), patch("speech.play_audio", side_effect=play):
            worker = SpeechWorker(self.config)
            try:
                worker.speak("toad", "old", "neutral", lambda: True)
                self.assertTrue(started.wait(2))
                worker.speak("toad", "new", "neutral", lambda: True)
                release.set()
                self.assertTrue(played.wait(2))
                self.assertEqual(said, [b"new"])
            finally:
                release.set()
                worker.close()

    def test_cancelled_context_does_not_play_after_download(self):
        def generate(config, voice, text, emotion, active):
            current.clear()
            return b"audio"
        current, done = threading.Event(), threading.Event()
        current.set()
        def synth(*args):
            result = generate(*args)
            done.set()
            return result
        with patch("speech.synthesize", side_effect=synth), patch("speech.play_audio") as play:
            worker = SpeechWorker(self.config)
            try:
                worker.speak("toad", "hello", "neutral", current.is_set)
                self.assertTrue(done.wait(2))
            finally:
                worker.close()
            play.assert_not_called()

    def test_voice_failure_is_separate_from_chat_and_hides_exception_details(self):
        logged = threading.Event()
        log = Mock(side_effect=lambda *args, **kw: logged.set())
        with patch("speech.synthesize", side_effect=RuntimeError("test-key")):
            worker = SpeechWorker(self.config, log=log)
            try:
                worker.speak("toad", "Hello!", "neutral", lambda: True)
                self.assertTrue(logged.wait(2))
                self.assertIn("text chat continues", log.call_args.args[0])
                self.assertNotIn("test-key", log.call_args.args[0])
            finally:
                worker.close()

    def test_windows_audio_commands_and_close_on_failure(self):
        commands = []
        def send(command, buffer, length, callback):
            commands.append(command)
            if command.startswith("status"):
                buffer.value = "stopped"
            return 0
        winmm = Mock()
        winmm.mciSendStringW.side_effect = send
        with patch("speech.ctypes.WinDLL", return_value=winmm, create=True):
            _play_windows(Path("voice.mp3"), lambda: True, 0.5)
        self.assertTrue(commands[0].startswith('open "'))
        self.assertIn("setaudio sm64npcvoice volume to 500", commands)
        self.assertEqual(commands[-1], "close sm64npcvoice")
        commands.clear()
        def fail(command, *args):
            commands.append(command)
            return 1 if command.startswith("play") else 0
        winmm.mciSendStringW.side_effect = fail
        with patch("speech.ctypes.WinDLL", return_value=winmm, create=True), self.assertRaises(RuntimeError):
            _play_windows(Path("voice.mp3"), lambda: True, 1)
        self.assertEqual(commands[-1], "close sm64npcvoice")

    def test_bridge_publishes_text_before_starting_voice(self):
        import bridge
        from test_bridge import request
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            request_path = root / ".sm64-npc-request.json"
            request_path.write_text(json.dumps(request()))
            worker = Mock()
            def speak(kind, text, emotion, current):
                self.assertTrue((root / ".sm64-npc-response").exists())
                self.assertEqual((kind, text, emotion), ("toad", "Hi Mario!", "excited"))
                self.assertTrue(current())
                request_path.unlink()
                self.assertFalse(current())
                raise KeyboardInterrupt
            worker.speak.side_effect = speak
            with patch.dict(os.environ, {"OPENAI_API_KEY": "test", "FISH_AUDIO_API_KEY": "test"}), \
                 patch("sys.argv", ["bridge.py", "--runtime-dir", str(root)]), \
                 patch.object(bridge, "ROOT", root), patch.object(bridge, "OpenAI"), \
                 patch.object(bridge, "SpeechWorker", return_value=worker), \
                 patch.object(bridge, "generate", return_value=bridge.NpcReply(
                     reply="Hi Mario!", action="none", coins=0, emotion="excited")), \
                 patch("builtins.print"):
                self.assertEqual(bridge.main(), 0)
                worker.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
