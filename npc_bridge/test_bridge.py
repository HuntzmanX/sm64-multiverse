import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from memory import acknowledge, load_memory, npc_memory, save_memory
from protocol import (ACTIONS, allowed_actions, ascii_text, atomic_write, request_from_file,
                      response_packet, validate_action)


def request(**changes):
    value = dict(id="123-1-10", npc="1:6:1:1:100:0:-200:0", kind="toad", save=1,
                 level=6, area=1, course=0, act=1, stars=12, dialog_id=82,
                 coin_budget=10, mode="none", can_open_cannon=False, player_text="Hello!")
    value.update(changes)
    return value


class ProtocolTests(unittest.TestCase):
    def test_request_roundtrip_and_atomic_replace(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "request.json"
            atomic_write(path, "{}")
            atomic_write(path, json.dumps(request(player_text='Say "hello" \\ please')))
            self.assertEqual(request_from_file(path)["player_text"], 'Say "hello" \\ please')
            self.assertFalse(path.with_name("request.json.tmp").exists())

    def test_reject_bad_requests(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "request.json"
            for changes in [dict(id="123\n4"), dict(npc="../../toad"), dict(kind="bowser"),
                            dict(stars=True), dict(coin_budget=11), dict(player_text=" "),
                            dict(player_text="x"*121), dict(can_open_cannon=1)]:
                path.write_text(json.dumps(request(**changes)))
                with self.subTest(changes=changes), self.assertRaises(ValueError):
                    request_from_file(path)

    def test_packet_sanitizes_newlines_and_smart_punctuation(self):
        self.assertEqual(response_packet("123-1-10", "I’m ready!\n5 3\nFollow me."),
                         "123-1-10\n0 0\nI'm ready! 5 3 Follow me.\n")
        self.assertLessEqual(len(ascii_text("x"*300)), 200)

    def test_reject_invalid_packets(self):
        for args in [("bad", "hi"), ("1-2-3", ""), ("1-2-3", "hi", "explode"),
                     ("1-2-3", "hi", "none", 1), ("1-2-3", "hi", "give_coins", 4)]:
            with self.subTest(args=args), self.assertRaises(ValueError):
                response_packet(*args)

    def test_wire_action_order(self):
        self.assertEqual(ACTIONS, ("none", "follow", "flee", "approach", "stop", "hostile", "friendly", "give_coins", "open_cannon"))

    def test_capabilities_are_per_npc(self):
        self.assertNotIn("hostile", allowed_actions(request()))
        self.assertNotIn("open_cannon", allowed_actions(request(kind="bobomb_buddy")))
        self.assertIn("open_cannon", allowed_actions(request(kind="bobomb_buddy", can_open_cannon=True)))
        with self.assertRaises(ValueError):
            validate_action(request(), "hostile", 0, 0)

    def test_reward_budget_checks_game_and_persistent_ledger(self):
        validate_action(request(), "give_coins", 2, 8)
        for req, amount, remembered in [(request(), 3, 8), (request(coin_budget=1), 2, 0),
                                        (request(), 0, 0), (request(), 1, 10)]:
            with self.subTest(amount=amount, remembered=remembered), self.assertRaises(ValueError):
                validate_action(req, "give_coins", amount, remembered)

    def test_coins_only_on_coin_action(self):
        with self.assertRaises(ValueError):
            validate_action(request(), "follow", 2, 0)


class MemoryTests(unittest.TestCase):
    def setUp(self):
        self.memory = {"npcs": {}, "pending": {"1-2-3": dict(npc="toad1", player="Please?",
            reply="Here you go!", action="give_coins", amount=3)}, "handled": []}
        self.receipt = dict(id="1-2-3", npc="toad1", action="give_coins", coins=2, result="Coins spawned")

    def test_no_rewards_or_history_until_acknowledged(self):
        self.assertEqual(npc_memory(self.memory, "toad1")["coins_given"], 0)
        self.assertEqual(npc_memory(self.memory, "toad1")["history"], [])
        self.assertTrue(acknowledge(self.memory, self.receipt))
        self.assertEqual(npc_memory(self.memory, "toad1")["coins_given"], 2)
        self.assertEqual(npc_memory(self.memory, "toad1")["history"][0]["actual_result"], "Coins spawned")

    def test_duplicate_ack_never_duplicates_reward(self):
        acknowledge(self.memory, self.receipt)
        self.assertFalse(acknowledge(self.memory, self.receipt))
        self.assertEqual(npc_memory(self.memory, "toad1")["coins_given"], 2)

    def test_wrong_npc_or_action_or_amount_not_accepted(self):
        for changes in [dict(npc="toad2"), dict(action="follow"), dict(coins=4), dict(coins=True), dict(id="old")]:
            with self.subTest(changes=changes):
                self.assertFalse(acknowledge(self.memory, self.receipt | changes))
        self.assertIn("1-2-3", self.memory["pending"])

    def test_memory_separation_and_persistence(self):
        acknowledge(self.memory, self.receipt)
        self.assertEqual(npc_memory(self.memory, "toad2")["history"], [])
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "memory.json"
            save_memory(path, self.memory)
            self.assertEqual(load_memory(path), self.memory)

    def test_corrupt_memory_does_not_silently_reset_budget(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "memory.json"
            path.write_text("{}")
            with self.assertRaises(ValueError):
                load_memory(path)


class ModelBoundaryTests(unittest.TestCase):
    def test_structured_api_reply_and_personality_context(self):
        from bridge import NpcReply, generate
        parse = unittest.mock.Mock(return_value=SimpleNamespace(
            output_parsed=NpcReply(reply="I'll come with you!", action="follow", coins=0, emotion="neutral", spoken_reply="I'll come with you!")))
        client = SimpleNamespace(responses=SimpleNamespace(parse=parse))
        entry = dict(coins_given=8, history=[dict(actual_result="Coins spawned")])
        result = generate(client, "test-model", request(), entry)
        self.assertEqual(result.action, "follow")
        call = parse.call_args.kwargs
        context = json.loads(call["input"][1]["content"])
        self.assertEqual(context["coin_budget"], 2)
        self.assertIn("hat", context["original_dialogue"])
        self.assertEqual(context["conversation_history"], entry["history"])
        self.assertIn("this individual Toad", call["input"][0]["content"])

    def test_model_cannot_invent_capabilities(self):
        from bridge import NpcReply, generate
        parse = unittest.mock.Mock(return_value=SimpleNamespace(
            output_parsed=NpcReply(reply="Kaboom!", action="hostile", coins=0, emotion="angry", spoken_reply="[angry] Kaboom!")))
        with self.assertRaises(ValueError):
            generate(SimpleNamespace(responses=SimpleNamespace(parse=parse)), "test", request(), dict(coins_given=0, history=[]))

    def test_spoken_directions_cannot_rewrite_displayed_words(self):
        from bridge import NpcReply
        good = NpcReply(reply="I am fine. Really!", action="none", coins=0, emotion="neutral",
                        spoken_reply="[nervous, forcing a smile] I am fine. [small laugh] Really!")
        self.assertEqual(good.reply, "I am fine. Really!")
        with self.assertRaises(ValueError):
            NpcReply(reply="Hello!", action="none", coins=0, emotion="happy", spoken_reply="[angry] Goodbye!")

    def test_superseded_request(self):
        from bridge import current_request
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "request.json"
            path.write_text(json.dumps(request()))
            self.assertTrue(current_request(path, "123-1-10"))
            self.assertFalse(current_request(path, "old"))
            path.unlink()
            self.assertFalse(current_request(path, "123-1-10"))

    def test_no_structured_output_is_an_error(self):
        from bridge import generate
        parse = unittest.mock.Mock(return_value=SimpleNamespace(output_parsed=None))
        with self.assertRaises(ValueError):
            generate(SimpleNamespace(responses=SimpleNamespace(parse=parse)), "test", request(), dict(coins_given=0, history=[]))


if __name__ == "__main__":
    unittest.main()
