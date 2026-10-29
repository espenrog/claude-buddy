"""The hourly battery log on the bridge side: decoding the characteristic, the CSV file, and fetching from a fake buddy."""

import asyncio
import importlib.util
import os
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BRIDGE = os.path.join(ROOT, "bridge", "claude_buddy_bridge.py")


def load_bridge():
    spec = importlib.util.spec_from_file_location("bridge_battery_log", BRIDGE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def record_wire(seq, hour=14, flags=0, percent=80, mv=3900, current_10ua=-110, lit_s=1800):
    return (seq.to_bytes(4, "little") + bytes([hour, flags, percent]) + mv.to_bytes(2, "little")
            + current_10ua.to_bytes(2, "little", signed=True) + lit_s.to_bytes(2, "little"))


def answer(newest, count, rec=b""):
    return newest.to_bytes(4, "little") + count.to_bytes(2, "little") + rec


class FakeBuddy:
    """Holds records by seq like the firmware does: write a seq, read the answer."""

    def __init__(self, records, count=None):
        self.records, self.selected = records, 0
        self.count = len(records) if count is None else count
        self.newest = max(records) if records else 0

    async def write_gatt_char(self, uuid, data, response=True):
        self.selected = int.from_bytes(data, "little")

    async def read_gatt_char(self, uuid):
        return answer(self.newest, self.count, self.records.get(self.selected, b""))


class DecodeTests(unittest.TestCase):
    def setUp(self):
        self.b = load_bridge()

    def test_header_only(self):
        self.assertEqual(self.b.unpack_log_answer(answer(7, 5)), (7, 5, None))

    def test_record(self):
        newest, count, rec = self.b.unpack_log_answer(answer(7, 5, record_wire(7, flags=1, current_10ua=-123)))
        self.assertEqual((newest, count), (7, 5))
        self.assertEqual(rec, {"seq": 7, "hour": 14, "on_usb": True, "percent": 80, "mv": 3900,
                               "avg_current_ma": -1.23, "display_on_min": 30.0})

    def test_unknown_hour_and_bad_length(self):
        _, _, rec = self.b.unpack_log_answer(answer(1, 1, record_wire(1, hour=0xFF)))
        self.assertIsNone(rec["hour"])
        with self.assertRaises(ValueError):
            self.b.unpack_log_answer(b"\x01\x02")


class FileTests(unittest.TestCase):
    def setUp(self):
        self.b = load_bridge()
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, "sub", "log.csv")

    def tearDown(self):
        self.dir.cleanup()

    def fetch(self, buddy):
        store = self.b.BatteryLogFile(self.path)
        return store, asyncio.run(self.b.fetch_battery_log(buddy, store))

    def lines(self):
        with open(self.path, encoding="utf-8") as f:
            return f.read().splitlines()

    def test_writes_header_and_rows(self):
        _, added = self.fetch(FakeBuddy({1: record_wire(1), 2: record_wire(2, flags=1, current_10ua=50)}))
        self.assertEqual(added, 2)
        rows = self.lines()
        self.assertEqual(rows[0], ",".join(self.b.BATTERY_LOG_COLUMNS))
        self.assertTrue(rows[1].startswith("0,1,14,"))
        first, second = rows[1].split(","), rows[2].split(",")
        self.assertEqual(first[4:], ["80", "3900", "-1.10", "1.10", "30.0", "0"])  # discharging: 1.10 mAh used in the hour
        self.assertEqual(second[4:], ["80", "3900", "0.50", "", "30.0", "1"])      # on USB: no use figure

    def test_only_new_records_are_fetched(self):
        buddy = FakeBuddy({1: record_wire(1), 2: record_wire(2)})
        self.fetch(buddy)
        buddy.records[3] = record_wire(3)
        buddy.newest, buddy.count = 3, 3
        _, added = self.fetch(buddy)
        self.assertEqual(added, 1)
        self.assertEqual(len(self.lines()), 4)
        _, added = self.fetch(buddy)
        self.assertEqual(added, 0)

    def test_overwritten_records_are_skipped(self):
        buddy = FakeBuddy({5: record_wire(5), 6: record_wire(6)}, count=2)  # 1..4 are gone
        _, added = self.fetch(buddy)
        self.assertEqual(added, 2)

    def test_numbering_starting_over_is_a_new_epoch(self):
        self.fetch(FakeBuddy({1: record_wire(1), 2: record_wire(2), 3: record_wire(3)}))
        _, added = self.fetch(FakeBuddy({1: record_wire(1, percent=99)}))
        self.assertEqual(added, 1)
        self.assertTrue(self.lines()[-1].startswith("1,1,"))

    def test_empty_log(self):
        _, added = self.fetch(FakeBuddy({}))
        self.assertEqual(added, 0)
        self.assertFalse(os.path.exists(self.path))


class MeasureModeTests(unittest.TestCase):
    def setUp(self):
        self.b = load_bridge()

    def test_wire(self):
        self.assertEqual(self.b.pack_measure("display_off"), bytes([1, 1]))
        self.assertEqual(self.b.unpack_measure(bytes([1, 4, 29])), ("stats", 29))
        with self.assertRaises(ValueError):
            self.b.pack_measure("warp")
        with self.assertRaises(ValueError):
            self.b.unpack_measure(bytes([1, 99, 0]))

    def test_modes_match_the_firmware_enum(self):
        with open(os.path.join(ROOT, "firmware", "src", "measure_mode.h"), encoding="utf-8") as f:
            text = f.read()
        body = text.split("enum measure_mode {")[1].split("};")[0]
        names = [line.split(",")[0].strip().replace("MEASURE_", "").lower() for line in body.splitlines() if line.strip().startswith("MEASURE_")]
        self.assertEqual(names[:-1], self.b.MEASURE_MODES)  # the last one is the count


if __name__ == "__main__":
    unittest.main()
