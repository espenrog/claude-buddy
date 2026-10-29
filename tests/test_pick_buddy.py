"""Choosing one buddy when several are in range."""

import importlib.util
import os
import unittest
from types import SimpleNamespace as NS

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("bridge", os.path.join(ROOT, "bridge", "claude_buddy_bridge.py"))
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


def buddy(name, address, rssi):
    return NS(name=name, address=address), NS(local_name=name, rssi=rssi)


A = buddy("Claude Buddy A1B2", "AA:00:00:00:A1:B2", -70)
B = buddy("Claude Buddy 0C3D", "AA:00:00:00:0C:3D", -50)


class PickBuddy(unittest.TestCase):
    def test_nothing_in_range(self):
        self.assertEqual(bridge.pick_buddy([]), (None, []))

    def test_single_buddy(self):
        self.assertEqual(bridge.pick_buddy([A]), (A[0], []))

    def test_nearest_wins_without_a_wish(self):
        device, others = bridge.pick_buddy([A, B])
        self.assertIs(device, B[0])
        self.assertEqual(others, ["Claude Buddy A1B2"])

    def test_wish_by_id_name_or_address(self):
        for wanted in ("a1b2", "Claude Buddy A1B2", "aa:00:00:00:a1:b2"):
            self.assertIs(bridge.pick_buddy([A, B], wanted)[0], A[0], wanted)

    def test_wished_buddy_not_in_range(self):
        self.assertEqual(bridge.pick_buddy([A, B], "FFFF"), (None, []))
        self.assertEqual(bridge.pick_buddy([A], "0C3D"), (None, []))

    def test_old_firmware_name_still_found(self):
        old = buddy("Claude Buddy", "AA:00:00:00:00:01", -60)
        self.assertIs(bridge.pick_buddy([old])[0], old[0])


if __name__ == "__main__":
    unittest.main()
