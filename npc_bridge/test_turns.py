import json
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

from protocol import atomic_write, event_packet, request_from_file
from speech import SpeechConfig
from test_bridge import request
from turns import ConversationTurn, GameEvents


class TurnTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.req = request(protocol_version=2, history_count=0)
        self.path = self.root / ".sm64-npc-request.json"
        self.path.write_text(json.dumps(self.req))
        self.pool = ThreadPoolExecutor(max_workers=2)
        self.addCleanup(self.pool.shutdown)
        self.reply = SimpleNamespace(reply="Hi Mario!", action="follow", coins=0, emotion="excited")
        self.entry = dict(history=[], coins_given=0)
        self.record = Mock()

    def turn(self, config=None):
        turn = ConversationTurn(self.req, self.root, config, self.pool, log=Mock())
        turn.events.emit = Mock()
        return turn

    def test_model_prepares_during_mario_tts_and_voices_play_in_order(self):
        started = threading.Event()
        events = []
        def generate():
            started.set()
            self.assertNotEqual(threading.current_thread(), threading.main_thread())
            return self.reply
        def prepare(config, kind, text, emotion, active, log):
            if kind == "mario":
                self.assertTrue(started.wait(2))
            return kind.encode()
        def play(audio, active, volume, on_start):
            events.append(f"play-{audio.decode()}")
            self.assertFalse((self.root / ".sm64-npc-response").exists())
            on_start()
            events.append(f"end-{audio.decode()}")
        turn = self.turn(SpeechConfig("test"))
        turn.events.emit.side_effect = lambda speaker, text: events.append(f"text-{speaker}")
        with patch("turns.prepare_voice", side_effect=prepare), patch("speech.play_audio", side_effect=play):
            self.assertTrue(turn.run(generate, self.entry, self.record))
        self.assertEqual(events, ["play-mario", "text-1", "end-mario", "play-toad", "text-2", "end-toad"])
        self.assertIn("1 0\nHi Mario!", (self.root / ".sm64-npc-response").read_text())
        self.record.assert_called_once_with(self.reply, "Hi Mario!")

    def test_text_only_always_reveals_both_speakers_in_order(self):
        turn = self.turn()
        with patch("speech.synthesize") as synth:
            self.assertTrue(turn.run(lambda: self.reply, self.entry, self.record))
        self.assertEqual(turn.events.emit.call_args_list[0].args, (1, "Hello!"))
        self.assertEqual(turn.events.emit.call_args_list[1].args, (2, "Hi Mario!"))
        synth.assert_not_called()

    def test_cancel_during_mario_drops_npc_and_action(self):
        turn = self.turn(SpeechConfig("test"))
        def play(audio, active, volume, on_start):
            on_start()
            self.path.unlink()
        with patch("turns.prepare_voice", return_value=b"audio"), patch("speech.play_audio", side_effect=play):
            self.assertFalse(turn.run(lambda: self.reply, self.entry, self.record))
        self.record.assert_not_called()
        self.assertFalse((self.root / ".sm64-npc-response").exists())
        self.assertEqual(turn.events.emit.call_count, 1)

    def test_model_error_still_leaves_player_and_readable_error(self):
        turn = self.turn()
        def generate():
            raise ValueError("model failed")
        with self.assertRaises(ValueError):
            turn.run(generate, self.entry, self.record)
        turn.fail()
        self.assertEqual(turn.events.emit.call_count, 2)
        self.assertEqual(turn.events.emit.call_args_list[0].args, (1, "Hello!"))
        self.assertIn("lost my train", turn.events.emit.call_args_list[1].args[1])
        self.assertIn("\n0 0\n", (self.root / ".sm64-npc-response").read_text())
        self.record.assert_not_called()

    def test_persistent_history_restored_before_current_turn(self):
        self.entry["history"] = [dict(player="Old question", reply="Old answer")]
        turn = self.turn()
        self.assertTrue(turn.run(lambda: self.reply, self.entry, self.record))
        self.assertEqual([c.args[0] for c in turn.events.emit.call_args_list], [3, 4, 1, 2])
        self.req["history_count"] = 2
        turn = self.turn()
        self.assertTrue(turn.run(lambda: self.reply, self.entry, self.record))
        self.assertEqual([c.args[0] for c in turn.events.emit.call_args_list], [1, 2])

    def test_stale_turn_does_not_infer_or_play(self):
        self.path.write_text(json.dumps(self.req | dict(id="1-9-9")))
        turn = self.turn(SpeechConfig("test"))
        generate = Mock()
        self.assertFalse(turn.run(generate, self.entry, self.record))
        generate.assert_not_called()
        turn.events.emit.assert_not_called()

    def test_reveal_packets_are_sanitized_and_validated(self):
        self.assertEqual(event_packet("1-2-3", 2, 1, "Hi\nMario!"), "1-2-3\n2 1\nHi Mario!\n")
        for token, sequence, speaker in [("bad", 1, 1), ("1-2-3", 0, 1), ("1-2-3", 1, 6)]:
            with self.assertRaises(ValueError):
                event_packet(token, sequence, speaker, "hi")

    def test_game_acknowledges_each_message_before_next_is_written(self):
        received, stopped = [], threading.Event()
        event_path = self.root / ".sm64-npc-event"
        ack_path = self.root / ".sm64-npc-event-ack"
        def game():
            while not stopped.wait(0.005):
                try:
                    lines = event_path.read_text().splitlines()
                    sequence = int(lines[1].split()[0])
                    received.append((sequence, lines[2]))
                    event_path.unlink()
                    atomic_write(ack_path, f"{lines[0]}\n{sequence}\n")
                except (OSError, ValueError, IndexError):
                    pass
        thread = threading.Thread(target=game)
        thread.start()
        try:
            events = GameEvents(self.req, self.root)
            events.emit(1, "Hello!")
            events.emit(2, "Hi Mario!")
        finally:
            stopped.set()
            thread.join(timeout=1)
        self.assertEqual(received, [(1, "Hello!"), (2, "Hi Mario!")])

    def test_bridge_uses_new_turn_controller_and_shuts_down(self):
        import bridge
        with patch.dict("os.environ", {"OPENAI_API_KEY": "test", "FISH_AUDIO_API_KEY": "test"}), \
             patch("sys.argv", ["bridge.py", "--runtime-dir", str(self.root)]), \
             patch.object(bridge, "ROOT", self.root), patch.object(bridge, "OpenAI"), \
             patch.object(bridge, "ConversationTurn") as cls, patch("builtins.print"):
            cls.return_value.run.side_effect = KeyboardInterrupt
            self.assertEqual(bridge.main(), 0)
            self.assertEqual(cls.call_args.args[2].voice_for("mario"), "a564085babeb4d14802ea31b7b6c2869")


if __name__ == "__main__":
    unittest.main()
