"""The bridge's side of pairing, with a stand-in for the Bluetooth client."""

import asyncio
import importlib.util
import os
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("bridge", os.path.join(ROOT, "bridge", "claude_buddy_bridge.py"))
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)

NEEDS = bytes([2, 0, 8, 0x1F, 10])  # version characteristic of a firmware that needs pairing
OPEN = bytes([2, 0, 8, 0x0F, 9])


class FakeClient:
    """Refuses the settings read until pair() has been called (when pairs_ok), like a buddy that wants encryption."""

    def __init__(self, pairs_ok=True, pair_error=None):
        self.paired, self.pairs_ok, self.pair_error = False, pairs_ok, pair_error
        self.pair_calls = 0

    async def read_gatt_char(self, uuid):
        if not self.paired:
            raise PermissionError("insufficient authentication")
        return b"\x00"

    async def pair(self):
        self.pair_calls += 1
        if self.pair_error:
            raise self.pair_error
        self.paired = self.pairs_ok


def run(coro):
    return asyncio.run(coro)


class SecureLink(unittest.TestCase):
    def setUp(self):
        bridge.STATE.pairing_hint = None
        real_sleep = asyncio.sleep
        self._sleep, bridge.asyncio.sleep = real_sleep, lambda *_: real_sleep(0)

    def tearDown(self):
        bridge.asyncio.sleep = self._sleep

    def test_old_firmware_is_left_alone(self):
        client = FakeClient()
        run(bridge.secure_link(client, OPEN))
        run(bridge.secure_link(client, None))
        self.assertEqual(client.pair_calls, 0)

    def test_pairs_when_refused_then_clears_the_hint(self):
        client = FakeClient()
        run(bridge.secure_link(client, NEEDS))
        self.assertEqual(client.pair_calls, 1)
        self.assertIsNone(bridge.STATE.pairing_hint)

    def test_already_paired_does_not_pair_again(self):
        client = FakeClient()
        client.paired = True
        run(bridge.secure_link(client, NEEDS))
        self.assertEqual(client.pair_calls, 0)

    def test_gives_up_with_a_hint_when_the_buddy_refuses(self):
        client = FakeClient(pairs_ok=False)
        with self.assertRaises(bridge.PairingNeeded):
            run(bridge.secure_link(client, NEEDS))
        self.assertIn("mute button", bridge.STATE.pairing_hint)
        self.assertEqual(bridge.STATE.snapshot()["pairing_hint"], bridge.STATE.pairing_hint)

    def test_platform_without_pair_support_still_retries(self):
        client = FakeClient(pair_error=NotImplementedError())
        with self.assertRaises(bridge.PairingNeeded):
            run(bridge.secure_link(client, NEEDS))

    def test_hint_survives_a_link_reset(self):
        bridge.STATE.pairing_hint = "x"
        bridge.STATE.reset_link()
        self.assertEqual(bridge.STATE.pairing_hint, "x")


if __name__ == "__main__":
    unittest.main()
