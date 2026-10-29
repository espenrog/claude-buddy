"""The bridge's web page and API (using the simulated buddy), settings validation, and the security rules."""

import asyncio
import collections
import http.client
import importlib.util
import io
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BRIDGE = os.path.join(ROOT, "bridge", "claude_buddy_bridge.py")


def load_bridge():
    spec = importlib.util.spec_from_file_location("bridge_web", BRIDGE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class SettingsFormatTests(unittest.TestCase):
    def setUp(self):
        self.b = load_bridge()

    def test_round_trip(self):
        values = {"brightness": 200, "sleep_brightness": 0, "face_profile": 2, "volume": 75,
                  "quiet_start_h": 23, "quiet_end_h": 6, "muted": True}
        data = self.b.pack_settings(values)
        self.assertEqual(len(data), 8)
        self.assertEqual(self.b.unpack_settings(data), values)

    def test_defaults_are_valid(self):
        self.assertEqual(self.b.unpack_settings(self.b.pack_settings(self.b.SETTINGS_DEFAULTS)),
                         self.b.SETTINGS_DEFAULTS)

    def test_invalid_values_are_explained(self):
        good = dict(self.b.SETTINGS_DEFAULTS)
        for key, bad, word in [("brightness", 0, "brightness"), ("brightness", 256, "brightness"),
                               ("face_profile", 3, "face_profile"), ("volume", 101, "volume"),
                               ("quiet_start_h", 24, "quiet_start_h"), ("sleep_brightness", -1, "sleep_brightness"),
                               ("brightness", "high", "brightness"), ("brightness", True, "brightness"),
                               ("brightness", 12.5, "brightness"), ("muted", 1, "muted")]:
            with self.assertRaises(ValueError, msg=f"{key}={bad!r}") as ctx:
                self.b.pack_settings({**good, key: bad})
            self.assertIn(word, str(ctx.exception))
        with self.assertRaises(ValueError):
            self.b.pack_settings({k: v for k, v in good.items() if k != "volume"})


class FirmwareAgreesTests(unittest.TestCase):
    """The Python and C validators must accept and reject the same settings."""

    def test_c_and_python_agree(self):
        cc = next((c for c in ("cc", "gcc", "clang") if shutil.which(c)), None)
        if cc is None:
            self.skipTest("no C compiler found")
        b = load_bridge()
        valid = [b.SETTINGS_DEFAULTS,
                 {"brightness": 1, "sleep_brightness": 0, "face_profile": 0, "volume": 0, "quiet_start_h": 0,
                  "quiet_end_h": 0, "muted": False},
                 {"brightness": 255, "sleep_brightness": 255, "face_profile": 2, "volume": 100, "quiet_start_h": 23,
                  "quiet_end_h": 23, "muted": True}]
        wire_invalid = ["01000a0000000000", "0180080064170700"[:14], "02800800 0000000000".replace(" ", ""),
                        "0180080301640000", "0180080064180000", "0180080064160080", "01800800640000",
                        "0180080065170700"]  # last one: volume 101
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "cfg_check.exe" if sys.platform == "win32" else "cfg_check")
            subprocess.run([cc, "-Wall", "-I", os.path.join(ROOT, "firmware", "src"),
                            os.path.join(ROOT, "firmware", "src", "buddy_cfg.c"),
                            os.path.join(ROOT, "firmware", "src", "battery.c"),
                            os.path.join(ROOT, "tests", "cfg_check.c"), "-o", exe], check=True)
            lines = [b.pack_settings(v).hex() for v in valid] + wire_invalid
            out = subprocess.run([exe], input="\n".join(lines) + "\n", capture_output=True, text=True).stdout.split()
        for line, got in zip(lines[:len(valid)], out):
            self.assertEqual(got, line, "firmware changed or rejected a setting that Python considers valid")
        for line, got in zip(lines[len(valid):], out[len(valid):]):
            self.assertEqual(got, "rejected", f"firmware accepted {line}")


class UsageStoreTests(unittest.TestCase):
    def store(self, path=None):
        self.clock = [1790000000.0]  # 2026-09-21 or thereabouts; moved by the tests
        return load_bridge().UsageStore(path, now=lambda: self.clock[0])

    def test_month_total_uses_the_largest_cost_per_session(self):
        u = self.store()
        for cost in (0.5, 1.5, 1.2):  # a lower number later (restart, missed update) must not reduce the total
            self.assertTrue(u.record("a", cost))
        u.record("b", 2.0)
        self.assertEqual(u.month_usd()[0], 3.5)

    def test_rejects_bad_input(self):
        u = self.store()
        for session, cost in (("", 1), ("a b", 1), ("a", -1), ("a", True), ("a", "1"), ("a", 1e9), ("a" * 65, 1)):
            self.assertFalse(u.record(session, cost), (session, cost))
        self.assertIsNone(u.snapshot())

    def test_today_counts_only_spend_since_midnight(self):
        u = self.store()
        u.record("a", 4.0)
        self.clock[0] += 86400
        u.record("a", 5.5)
        self.assertEqual(u.today_usd(), 1.5)
        self.assertEqual(u.month_usd()[0], 5.5)

    def test_calibration_sets_the_month_and_follows_new_spend(self):
        u = self.store()
        u.record("a", 10.0)
        u.calibrate(14.96)
        self.assertEqual(u.snapshot()["month_usd"], 14.96)
        u.record("a", 11.0)
        self.assertEqual(u.snapshot()["month_usd"], 15.96)

    def test_calibration_learns_the_ratio_from_two_figures(self):
        u = self.store()
        u.record("a", 100.0)
        u.calibrate(140.0)  # first time: no ratio yet, spend after it counts at list price
        u.record("a", 110.0)
        self.assertEqual(u.snapshot()["month_usd"], 150.0)
        u.calibrate(158.0)  # the app grew 18 while the estimate grew 10
        self.assertEqual(u.calib[u._stamp()[0]]["factor"], 1.8)
        u.record("a", 120.0)
        self.assertEqual(u.snapshot()["month_usd"], 176.0)
        u.calibrate(176.0)  # the app's figure grew exactly as predicted: the ratio is confirmed
        self.assertEqual(u.calib[u._stamp()[0]]["factor"], 1.8)
        u.record("a", 122.0)  # two dollars of spend: too little to measure a ratio from
        u.calibrate(180.0)
        self.assertEqual(u.calib[u._stamp()[0]]["factor"], 1.8)

    def test_old_offsets_still_load(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "usage.json")
            u = self.store(path)
            month = u._stamp()[0]
            with open(path, "w") as f:
                json.dump({"sessions": {"a": {"max": 10.0, "month": month, "day": u._stamp()[1], "day_start": 0.0}},
                           "offsets": {month: 4.96}}, f)
            self.assertEqual(self.store(path).snapshot()["month_usd"], 14.96)

    def test_budget_warning(self):
        u = self.store()
        u.set_budget(100, 80)
        u.record("a", 79.0)
        self.assertFalse(u.snapshot()["over_warning"])
        u.record("a", 80.0)
        snap = u.snapshot()
        self.assertTrue(snap["over_warning"])
        self.assertEqual(snap["budget_used_pct"], 80)
        for bad in ((-1, 80), (10, 0), (10, 101), (10, 50.5), ("x", 80), (True, 80)):
            with self.assertRaises(ValueError):
                u.set_budget(*bad)

    def test_survives_a_restart(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "sub", "usage.json")
            u = self.store(path)
            u.record("a", 3.0, 42)
            u.set_budget(50, 90)
            u.calibrate(5.0)
            again = self.store(path)
            snap = again.snapshot()
            self.assertEqual((snap["month_usd"], snap["budget_usd"], snap["warn_pct"]), (5.0, 50.0, 90))
            self.assertEqual(snap["context_pct"], 42)

    def test_ingest_parses_relay_messages(self):
        b = load_bridge()
        b.USAGE = b.UsageStore(None)
        for text, ok in (("usage:abc:1.25:40", True), ("usage:abc:2", True), ("usage:abc:3:", True),
                         ("usage:abc", False), ("usage:abc:x", False), ("usage:a:b:c:d", False), ("usage:bad id:1", False)):
            self.assertEqual(bool(b.ingest(text, None)), ok, text)
        self.assertEqual(b.USAGE.month_usd()[0], 3.0)


class StatusLineTests(unittest.TestCase):
    def run_statusline(self, payload, cfg):
        with tempfile.TemporaryDirectory() as tmp:
            config = os.path.join(tmp, "config.json")
            with open(config, "w", encoding="utf-8") as f:
                json.dump(cfg, f)
            env = {**os.environ, "CLAUDE_BUDDY_CONFIG": config, "TMPDIR": tmp, "TEMP": tmp, "TMP": tmp}
            return subprocess.run([sys.executable, os.path.join(ROOT, "hooks", "statusline.py")],
                                  input=json.dumps(payload), capture_output=True, text=True, env=env, timeout=20)

    def test_chain_output_is_passed_through_even_without_a_bridge(self):
        port = free_port()  # nothing listens here
        cfg = {"http_port": port, "statusline": {"chain": f'"{sys.executable}" -c "import sys; print(\'mine\', len(sys.stdin.read()) > 5)"'}}
        out = self.run_statusline({"session_id": "s1", "cost": {"total_cost_usd": 1.5}}, cfg)
        self.assertEqual(out.returncode, 0)
        self.assertEqual(out.stdout.strip(), "mine True")

    def test_garbage_input_is_harmless(self):
        out = self.run_statusline({"nothing": "useful"}, {"http_port": free_port()})
        self.assertEqual((out.returncode, out.stdout), (0, ""))


class UsageWireTests(unittest.TestCase):
    def test_wire_format_1(self):
        b = load_bridge()
        u = b.UsageStore(None)
        self.assertEqual(u.wire(), bytes([1, 0, 0]))  # nothing known: no budget
        u.set_budget(100, 80)
        u.record("a", 50.0)
        self.assertEqual(u.wire(), bytes([1, 1, 50]))
        u.record("a", 85.0)
        self.assertEqual(u.wire(), bytes([1, 3, 85]))
        u.record("b", 900.0)
        self.assertEqual(u.wire(), bytes([1, 3, 255]))  # saturates
        u.set_budget(0, 80)
        self.assertEqual(u.wire(), bytes([1, 0, 0]))

    def test_wire_format_2_carries_the_amounts(self):
        b = load_bridge()
        u = b.UsageStore(None)
        self.assertEqual(u.wire(True), bytes([2, 0, 0, 0, 0, 0, 0, 0, 0]))
        u.set_budget(200, 80)
        u.record("a", 84.21)
        wire = u.wire(True)
        self.assertEqual(wire[:3], bytes([2, 1, 42]))
        self.assertEqual(int.from_bytes(wire[3:7], "little"), 8421)
        self.assertEqual(int.from_bytes(wire[7:9], "little"), 8421)  # all of it was today
        u.record("b", 900.0)  # more than 16 bits of cents today
        self.assertEqual(int.from_bytes(u.wire(True)[7:9], "little"), 65535)

    def test_c_and_python_agree(self):
        cc = next((c for c in ("cc", "gcc", "clang") if shutil.which(c)), None)
        if cc is None:
            self.skipTest("no C compiler found")
        b = load_bridge()
        valid = ["010000", "010100", "010350", "0103ff", "010164", "020000" + "00000000" + "0000",
                 "020164" + "ffffffff" + "ffff"]
        invalid = ["01", "0100", "01000000", "000100", "030100", "010400", "010800", "010001", "010200",
                   "0200000000000000", "02000000000000000000" + "00", "020001" + "00000000" + "0000",
                   "0203" + "00" + "00000000" + "00"]  # last: too short
        u = b.UsageStore(None)
        u.set_budget(10, 50)
        u.record("a", 7.0)
        valid += [u.wire().hex(), u.wire(True).hex()]
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "cfg_check.exe" if sys.platform == "win32" else "cfg_check")
            subprocess.run([cc, "-Wall", "-I", os.path.join(ROOT, "firmware", "src"),
                            os.path.join(ROOT, "firmware", "src", "buddy_cfg.c"),
                            os.path.join(ROOT, "firmware", "src", "battery.c"),
                            os.path.join(ROOT, "tests", "cfg_check.c"), "-o", exe], check=True)
            out = subprocess.run([exe, "usage"], input="\n".join(valid + invalid) + "\n",
                                 capture_output=True, text=True).stdout.split()
        self.assertEqual(out[:len(valid)], valid)
        self.assertEqual(set(out[len(valid):]), {"rejected"}, out)


class BatteryWireTests(unittest.TestCase):
    GOOD = {"present": True, "charging": False, "full": False, "percent": 73, "mv": 3916}

    def test_round_trip(self):
        b = load_bridge()
        data = b.pack_battery(self.GOOD)
        self.assertEqual(data, bytes([1, 1, 73, 0x4c, 0x0f]))
        self.assertEqual(b.unpack_battery(data), {**self.GOOD, "current_ma": None, "die_temp_c": None, "cell_temp_c": None})
        gone = {"present": False, "charging": False, "full": False, "percent": 0, "mv": 0}
        self.assertEqual(b.unpack_battery(b.pack_battery(gone)), {**gone, "current_ma": None, "die_temp_c": None, "cell_temp_c": None})

    def test_current_and_die_temperature(self):
        b = load_bridge()
        base = b.pack_battery(self.GOOD)
        charging = b.unpack_battery(base + (480).to_bytes(2, "little", signed=True) + (31).to_bytes(1, "little", signed=True))
        self.assertEqual((charging["current_ma"], charging["die_temp_c"]), (480, 31))
        drawing = b.unpack_battery(base + (-12).to_bytes(2, "little", signed=True) + (-5).to_bytes(1, "little", signed=True))
        self.assertEqual((drawing["current_ma"], drawing["die_temp_c"]), (-12, -5))
        unknown = b.unpack_battery(base + bytes([0x00, 0x80, 0x80]))
        self.assertEqual((unknown["current_ma"], unknown["die_temp_c"]), (None, None))
        self.assertEqual(unknown["percent"], 73)
        full = b.unpack_battery(base + bytes([0xe0, 0x01, 31, 0xfd]))  # eight bytes plus the cell temperature, -3 C
        self.assertEqual((full["current_ma"], full["die_temp_c"], full["cell_temp_c"]), (480, 31, -3))
        old = b.unpack_battery(base + bytes([0xe0, 0x01, 31]))  # firmware 0.8.5: no cell temperature
        self.assertEqual((old["die_temp_c"], old["cell_temp_c"]), (31, None))
        self.assertIsNone(b.unpack_battery(base + bytes([0xe0, 0x01, 31, 0x80]))["cell_temp_c"])
        for bad in (base + b"\x00", base + bytes(5)):
            with self.assertRaises(ValueError):
                b.unpack_battery(bad)

    def test_rejections(self):
        b = load_bridge()
        for change in ({"percent": 101}, {"percent": -1}, {"percent": 5.5}, {"mv": 5001}, {"charging": True, "full": True},
                       {"present": False}, {"present": "yes"}, {"percent": True}):
            with self.assertRaises(ValueError, msg=str(change)):
                b.pack_battery({**self.GOOD, **change})

    def test_c_and_python_agree(self):
        cc = next((c for c in ("cc", "gcc", "clang") if shutil.which(c)), None)
        if cc is None:
            self.skipTest("no C compiler found")
        b = load_bridge()
        valid = [b.pack_battery(self.GOOD), b.pack_battery({**self.GOOD, "charging": True}),
                 b.pack_battery({**self.GOOD, "full": True, "percent": 100}),
                 b.pack_battery({"present": False, "charging": False, "full": False, "percent": 0, "mv": 0})]
        invalid = ["0201" + "000000", "010800" + "0000", "01016500" + "00", "0107" + "320000", "0106320000", "0100050000",
                   "0100000100", "01013200", "010132000000", "0101" + "32" + "8913"]
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "cfg_check.exe" if sys.platform == "win32" else "cfg_check")
            subprocess.run([cc, "-Wall", "-I", os.path.join(ROOT, "firmware", "src"),
                            os.path.join(ROOT, "firmware", "src", "buddy_cfg.c"),
                            os.path.join(ROOT, "firmware", "src", "battery.c"),
                            os.path.join(ROOT, "tests", "cfg_check.c"), "-o", exe], check=True)
            lines = [v.hex() for v in valid] + invalid
            out = subprocess.run([exe, "battery"], input="\n".join(lines) + "\n", capture_output=True,
                                 text=True).stdout.split()
        self.assertEqual(out[:len(valid)], [v.hex() for v in valid])
        self.assertEqual(set(out[len(valid):]), {"rejected"}, out)


class TimeWireTests(unittest.TestCase):
    def test_time_wire(self):
        b = load_bridge()
        stamp = time.mktime((2026, 10, 1, 23, 7, 30, 0, 0, -1))
        self.assertEqual(b.time_wire(stamp), bytes([1, 23, 7]))
        self.assertEqual(b.time_wire(stamp + 53 * 60), bytes([1, 0, 0]))
        self.assertEqual(len(b.time_wire()), 3)


class FirmwareLogicTests(unittest.TestCase):
    """The firmware's pure logic (when the picture can change, sounds, clock) compiled and run on the host."""

    def test_logic_check(self):
        cc = next((c for c in ("cc", "gcc", "clang") if shutil.which(c)), None)
        if cc is None:
            self.skipTest("no C compiler found")
        src = os.path.join(ROOT, "firmware", "src")
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "logic_check.exe" if sys.platform == "win32" else "logic_check")
            subprocess.run([cc, "-Wall", "-Wextra", "-Werror", "-I", src] +
                           [os.path.join(src, f) for f in ("face.c", "mood.c", "buddy_cfg.c", "sound.c", "buddy_clock.c", "battery.c")] +
                           [os.path.join(ROOT, "tests", "logic_check.c"), "-o", exe], check=True)
            result = subprocess.run([exe], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout)


class FacePreviewTests(unittest.TestCase):
    """Draws every mood and the stats page with the firmware's own drawing code (no hardware, no crashes)."""

    def test_every_picture_renders(self):
        cc = next((c for c in ("cc", "gcc", "clang") if shutil.which(c)), None)
        if cc is None:
            self.skipTest("no C compiler found")
        src = os.path.join(ROOT, "firmware", "src")
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "face_preview.exe" if sys.platform == "win32" else "face_preview")
            subprocess.run([cc, "-Wall", "-Werror", "-I", src, os.path.join(src, "face.c"), os.path.join(src, "mood.c"),
                            os.path.join(src, "buddy_cfg.c"), os.path.join(src, "battery.c"),
                            os.path.join(ROOT, "tests", "face_preview.c"), "-o", exe],
                           check=True)
            out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout
        pictures = out.split("--- ")[1:]
        self.assertEqual(len(pictures), 11 + 1 + 1 + 4 + 5)
        for pic in pictures:
            title, *rows = pic.splitlines()
            self.assertEqual(len(rows), 64, title)
            self.assertTrue(all(len(r) == 128 for r in rows), title)
            self.assertIn("#", pic, title)
        # the mouth: chill has one under the eyes, limit does not
        chill = next(p for p in pictures if p.startswith("chill\n")).splitlines()[1:]
        self.assertIn("#", "".join(chill[44:56]))


class WebApiTests(unittest.TestCase):
    simulate = True

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.port = free_port()
        config = os.path.join(self.tmp.name, "config.json")
        with open(config, "w", encoding="utf-8") as f:
            json.dump({"http_port": self.port, "usage_file": os.path.join(self.tmp.name, "usage.json"),
                       "bridge": {"inputs": ["http"], "ble": False}}, f)
        log = os.path.join(self.tmp.name, "bridge.log")
        args = [sys.executable, BRIDGE, "--config", config, "--log-file", log]
        args.append("--simulate" if self.simulate else "--no-ble")
        self.proc = subprocess.Popen(args)
        end = time.time() + 10
        while time.time() < end:
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.5).close()
                break
            except OSError:
                time.sleep(0.1)
        else:
            self.fail("bridge did not start")

    def tearDown(self):
        self.proc.terminate()
        self.proc.wait(timeout=10)
        self.tmp.cleanup()

    def request(self, method, path, body=None, headers=None, host=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        hdrs = dict(headers or {})
        conn.putrequest(method, path, skip_host=True)
        conn.putheader("Host", host or f"127.0.0.1:{self.port}")
        for k, v in hdrs.items():
            conn.putheader(k, v)
        data = None if body is None else (body if isinstance(body, bytes) else json.dumps(body).encode())
        conn.putheader("Content-Length", str(len(data or b"")))
        conn.endheaders(data)
        resp = conn.getresponse()
        raw = resp.read()
        conn.close()
        try:
            return resp.status, json.loads(raw) if raw else None, resp
        except ValueError:
            return resp.status, raw, resp

    API = {"X-Claude-Buddy": "1", "Content-Type": "application/json"}

    def status(self):
        return self.request("GET", "/api/status")[1]

    def wait_status(self, check, seconds=5):
        end = time.time() + seconds
        while time.time() < end:
            st = self.status()
            if check(st):
                return st
            time.sleep(0.1)
        self.fail(f"status never matched: {self.status()}")


class SimulatedBuddyTests(WebApiTests):
    def test_the_page_is_served_with_a_strict_policy(self):
        status, body, resp = self.request("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn(b"Claude Buddy", body)
        self.assertIn("default-src 'none'", resp.getheader("Content-Security-Policy"))
        self.assertEqual(resp.getheader("X-Frame-Options"), "DENY")

    def test_status_reports_the_simulated_buddy(self):
        st = self.status()
        self.assertTrue(st["ble"] and st["simulated"])
        self.assertEqual(st["settings"]["brightness"], 128)
        self.assertEqual(st["mood"], "chill")

    def test_settings_can_be_changed(self):
        new = {"brightness": 77, "sleep_brightness": 0, "face_profile": 1, "volume": 10, "quiet_start_h": 1,
               "quiet_end_h": 2, "muted": True}
        status, body, _ = self.request("POST", "/api/settings", new, self.API)
        self.assertEqual(status, 202, body)
        self.assertEqual(self.wait_status(lambda s: s["settings"]["brightness"] == 77)["settings"], new)

    def test_invalid_settings_are_rejected_with_a_message(self):
        bad = {"brightness": 0, "sleep_brightness": 8, "face_profile": 0, "volume": 60, "quiet_start_h": 22,
               "quiet_end_h": 7, "muted": False}
        status, body, _ = self.request("POST", "/api/settings", bad, self.API)
        self.assertEqual(status, 400)
        self.assertIn("brightness", body["error"])
        self.assertEqual(self.request("POST", "/api/settings", b"not json", self.API)[0], 400)
        self.assertEqual(self.status()["settings"]["brightness"], 128)

    def test_test_buttons_send_events_and_move_the_simulated_mood(self):
        status, _, _ = self.request("POST", "/api/test/needs_input", None, self.API)
        self.assertEqual(status, 202)
        st = self.wait_status(lambda s: s["mood"] == "alert")
        self.assertEqual(st["events"][0]["event"], "needs_input")
        self.assertEqual(self.request("POST", "/api/test/nonsense", None, self.API)[0], 400)

    def test_hook_endpoint_still_works(self):
        self.assertEqual(self.request("POST", "/e/turn_finished/some-session")[0], 204)
        self.wait_status(lambda s: s["events"] and s["events"][0]["event"] == "turn_finished")


class SecurityTests(SimulatedBuddyTests):
    def test_api_posts_need_the_custom_header(self):
        self.assertEqual(self.request("POST", "/api/test/idle")[0], 403)
        self.assertEqual(self.request("POST", "/api/settings", {"brightness": 5})[0], 403)

    def test_requests_for_other_host_names_are_refused(self):  # DNS rebinding
        for path in ("/", "/api/status"):
            self.assertEqual(self.request("GET", path, host="evil.example.com")[0], 403)
        self.assertEqual(self.request("POST", "/e/idle", host="evil.example.com")[0], 403)

    def test_requests_from_other_sites_are_refused(self):  # cross-site request forgery
        evil = {"Origin": "https://evil.example.com"}
        self.assertEqual(self.request("GET", "/api/status", headers=evil)[0], 403)
        self.assertEqual(self.request("POST", "/e/idle", headers=evil)[0], 403)
        self.assertEqual(self.request("POST", "/api/test/idle", None, {**self.API, **evil})[0], 403)

    def test_requests_from_our_own_page_are_allowed(self):
        own = {"Origin": f"http://127.0.0.1:{self.port}"}
        self.assertEqual(self.request("GET", "/api/status", headers=own)[0], 200)

    def test_unknown_paths_and_methods(self):
        self.assertEqual(self.request("GET", "/nope")[0], 404)
        self.assertEqual(self.request("GET", "/e/idle")[0], 404)
        self.assertEqual(self.request("GET", "/api/settings")[0], 405)
        self.assertEqual(self.request("POST", "/api/unknown", None, self.API)[0], 404)

    def test_an_oversized_body_is_not_accepted_as_settings(self):
        self.assertEqual(self.request("POST", "/api/settings", b"{" + b" " * 10000, self.API)[0], 400)


class NoBuddyConnectedTests(WebApiTests):
    simulate = False

    def test_settings_cannot_be_written_without_a_buddy(self):
        ok = {"brightness": 100, "sleep_brightness": 8, "face_profile": 0, "volume": 60, "quiet_start_h": 22,
              "quiet_end_h": 7, "muted": False}
        status, body, _ = self.request("POST", "/api/settings", ok, self.API)
        self.assertEqual(status, 503)
        self.assertIn("not connected", body["error"])
        self.assertFalse(self.status()["ble"])


if __name__ == "__main__":
    unittest.main()


class UsageApiTests(WebApiTests):
    """The status line feeds /usage/..., the web page reads it from /api/status and edits budget and calibration."""

    def test_nothing_reported_before_any_usage(self):
        self.assertIsNone(self.status()["usage"])

    def test_usage_budget_and_calibration(self):
        self.assertEqual(self.request("POST", "/usage/s1/12.5/33")[0], 204)
        self.assertEqual(self.request("POST", "/usage/s2/2.5")[0], 204)
        self.assertEqual(self.request("POST", "/usage/s1/-3")[0], 400)
        self.assertEqual(self.request("POST", "/usage/s1/abc")[0], 400)
        self.assertEqual(self.request("GET", "/usage/s1/1")[0], 400)  # only POST
        usage = self.status()["usage"]
        self.assertEqual((usage["month_usd"], usage["today_usd"], usage["context_pct"]), (15.0, 15.0, 33))
        code, body, _ = self.request("POST", "/api/budget", {"monthly_usd": 20, "warn_pct": 70}, self.API)
        self.assertEqual(code, 200)
        self.assertTrue(body["usage"]["over_warning"])
        self.assertEqual(self.request("POST", "/api/budget", {"monthly_usd": -5}, self.API)[0], 400)
        code, body, _ = self.request("POST", "/api/calibrate", {"month_usd": 100}, self.API)
        self.assertEqual((code, body["usage"]["month_usd"]), (200, 100.0))
        self.assertEqual(self.request("POST", "/api/calibrate", {"month_usd": "lots"}, self.API)[0], 400)

    def test_changing_things_needs_the_header_and_a_local_page(self):
        self.assertEqual(self.request("POST", "/api/budget", {"monthly_usd": 5})[0], 403)
        self.assertEqual(self.request("POST", "/api/calibrate", {"month_usd": 5},
                                      {**self.API, "Origin": "http://evil.example"})[0], 403)


class BatteryApiTests(WebApiTests):
    def test_battery_test_values(self):
        self.assertIsNone(self.status()["battery"])
        self.assertTrue(self.status()["can_simulate_battery"])  # the simulated buddy has no real gauge
        good = {"present": True, "charging": True, "full": False, "percent": 40, "mv": 3830}
        self.assertEqual(self.request("POST", "/api/battery", good, self.API)[0], 202)
        deadline = time.time() + 5
        while time.time() < deadline and self.status()["battery"] is None:
            time.sleep(0.1)
        self.assertEqual(self.status()["battery"], {**good, "current_ma": None, "die_temp_c": None, "cell_temp_c": None})
        code, body, _ = self.request("POST", "/api/battery", {**good, "percent": 101}, self.API)
        self.assertEqual(code, 400)
        self.assertIn("percent", body["error"])
        self.assertEqual(self.request("POST", "/api/battery", good)[0], 403)  # needs the header


def fake_package(image=None, manifest=None, name="firmware.signed.bin"):
    """A DFU zip like the one the build makes, around a stand-in application image."""
    image = bytes.fromhex("3db8f396") + bytes(1000) if image is None else image
    manifest = manifest if manifest is not None else {"format-version": 1, "files": [
        {"type": "application", "image_index": "0", "file": name, "size": len(image)}]}
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as zf:
        zf.writestr("manifest.json", json.dumps(manifest))
        zf.writestr(name, image)
    return out.getvalue()


class UpdatePackageTests(unittest.TestCase):
    def setUp(self):
        self.b = load_bridge()
        self.magic = bytes.fromhex("3db8f396")

    def test_zip_and_bare_image_both_give_the_image(self):
        image = self.magic + bytes(500)
        self.assertEqual(self.b.extract_app_image(fake_package(image)), image)
        self.assertEqual(self.b.extract_app_image(image), image)

    def test_rejects_what_is_not_a_signed_application(self):
        for bad, word in [(b"", "empty"), (b"hello world", "signed"), (b"PK\x03\x04junk", "firmware package"),
                          (fake_package(b"not an image"), "signed"),
                          (fake_package(manifest={"files": []}), "no images"),
                          (fake_package(manifest={"files": [{"image_index": "1", "file": "x"}]}), "more than"),
                          (fake_package(manifest={"files": [{"image_index": "0", "file": "missing.bin"}]}), "firmware package"),
                          (self.magic + bytes(self.b.APP_SLOT_BYTES), "slot")]:
            with self.assertRaises(ValueError, msg=repr(bad[:12])) as ctx:
                self.b.extract_app_image(bad)
            self.assertIn(word, str(ctx.exception))

    def test_the_outcome_is_cleared_after_a_while(self):
        b = self.b
        for state, shown_for in (("done", b.UPDATE.SHOW_DONE_S), ("error", b.UPDATE.SHOW_ERROR_S)):
            b.UPDATE.set(state, "outcome")
            self.assertEqual(b.UPDATE.snapshot()["state"], state)
            b.UPDATE.since -= shown_for + 1
            st = b.UPDATE.snapshot()
            self.assertEqual((st["state"], st["message"]), ("idle", ""))

    def run_update(self, upload, ble=False):
        b = self.b
        b.STATE.reset_link()
        b.STATE.ble, b.STATE.can_update = ble, ble
        b.UPDATE.set("preparing")
        image = self.magic + bytes(100)
        asyncio.run(b.run_update(image, upload))
        return b.UPDATE.snapshot()

    def test_a_good_upload_reports_done(self):
        sent = []

        async def upload(image, progress, address):
            progress(50)
            sent.append(len(image))
        st = self.run_update(upload)
        self.assertEqual((st["state"], sent), ("done", [104]))
        self.assertFalse(st["busy"])

    def test_failures_are_reported_not_raised(self):
        async def refused(image, progress, address):
            raise self.b.UpdateError("the buddy refused the new image")

        async def broken(image, progress, address):
            raise RuntimeError("link lost")
        st = self.run_update(refused)
        self.assertEqual((st["state"], st["message"]), ("error", "the buddy refused the new image"))
        st = self.run_update(broken)
        self.assertEqual(st["state"], "error")
        self.assertIn("link lost", st["message"])

    def test_the_buddy_gets_the_image_over_a_new_link(self):
        b = self.b
        b.STATE.reset_link()
        b.STATE.ble, b.STATE.can_update = True, True
        b.STATE.address = "AA:BB:CC:DD:EE:FF"
        b.UPDATE.set("preparing")
        called = []

        async def upload(image, progress, address):
            called.append(address)

        async def scenario():
            task = asyncio.ensure_future(b.run_update(self.magic + bytes(8), upload))
            await asyncio.sleep(0.1)
            self.assertEqual(b.UPDATE.state, "releasing")  # the BLE loop lets go of the link on this
            self.assertFalse(called)
            b.STATE.reset_link()  # what the BLE loop does when it disconnects
            await task
        asyncio.run(scenario())
        self.assertEqual((b.UPDATE.state, called), ("done", ["AA:BB:CC:DD:EE:FF"]))
        self.assertIn("goes back to the old one", b.UPDATE.message)

    def test_the_new_image_is_found_in_the_second_slot(self):
        image = collections.namedtuple("Image", "slot hash")
        self.assertEqual(self.b.pending_image_hash([image(0, b"old"), image(1, b"new")]), b"new")
        self.assertIsNone(self.b.pending_image_hash([image(0, b"old")]))
        self.assertIsNone(self.b.pending_image_hash(None))

    def test_a_buddy_that_cannot_be_updated_is_refused(self):
        b = self.b
        b.UPDATE.set("idle")
        ctx = b.WebContext(8787, None, None)  # the refusals never reach the queues
        headers = {"x-claude-buddy": "1", "host": "127.0.0.1:8787"}
        for ble, can_update, code in ((False, False, b"503"), (True, False, b"409")):
            b.STATE.reset_link()
            b.STATE.ble, b.STATE.can_update = ble, can_update
            reply = b.route("POST", "/api/update", headers, fake_package(), ctx)
            self.assertIn(code, reply.split(b"\r\n", 1)[0])
        self.assertEqual(b.UPDATE.state, "idle")


class UpdateUploadApiTests(WebApiTests):
    UPLOAD = {"X-Claude-Buddy": "1", "Content-Type": "application/octet-stream"}

    def test_a_package_is_accepted_and_reaches_done(self):
        status, body, _ = self.request("POST", "/api/update", fake_package(), self.UPLOAD)
        self.assertEqual((status, body["ok"]), (202, True))
        st = self.wait_status(lambda s: s["update"]["state"] == "done")
        self.assertEqual(st["update"]["percent"], 100)

    def test_bad_files_are_refused_with_a_reason(self):
        status, body, _ = self.request("POST", "/api/update", b"not firmware", self.UPLOAD)
        self.assertEqual(status, 400)
        self.assertIn("signed", body["error"])
        self.assertEqual(self.status()["update"]["state"], "idle")

    def test_needs_the_header_and_the_right_origin(self):
        self.assertEqual(self.request("POST", "/api/update", fake_package())[0], 403)
        self.assertEqual(self.request("POST", "/api/update", fake_package(), {**self.UPLOAD, "Origin": "http://evil.example"})[0], 403)
        self.assertEqual(self.request("GET", "/api/update")[0], 405)

    def test_a_second_upload_while_one_runs_is_refused(self):
        self.assertEqual(self.request("POST", "/api/update", fake_package(), self.UPLOAD)[0], 202)
        status, body, _ = self.request("POST", "/api/update", fake_package(), self.UPLOAD)  # the simulated upload takes 0.5 s
        self.assertEqual(status, 409)
        self.assertIn("already", body["error"])
        self.wait_status(lambda s: s["update"]["state"] == "done")


class MeasureApiTests(WebApiTests):
    def test_modes_can_be_held_and_released(self):
        self.assertTrue(self.status()["can_measure"])
        self.assertIsNone(self.status()["measure"])
        self.assertEqual(self.request("POST", "/api/measure", {"mode": "display_off"}, self.API)[0], 202)
        st = self.wait_status(lambda s: s["measure"] is not None)
        self.assertEqual(st["measure"]["mode"], "display_off")
        self.assertIn(st["measure"]["minutes_left"], (29, 30))
        self.assertTrue(st["display_off"])
        self.assertEqual(self.request("POST", "/api/measure", {"mode": "normal"}, self.API)[0], 202)
        self.wait_status(lambda s: s["measure"] is None)

    def test_beep_does_not_change_the_held_mode(self):
        self.request("POST", "/api/measure", {"mode": "stats"}, self.API)
        self.wait_status(lambda s: s["measure"] is not None)
        self.assertEqual(self.request("POST", "/api/measure", {"mode": "beep"}, self.API)[0], 202)
        time.sleep(0.6)
        self.assertEqual(self.status()["measure"]["mode"], "stats")

    def test_bad_requests(self):
        code, body, _ = self.request("POST", "/api/measure", {"mode": "warp"}, self.API)
        self.assertEqual(code, 400)
        self.assertIn("mode", body["error"])
        self.assertEqual(self.request("POST", "/api/measure", {"mode": "stats"})[0], 403)  # needs the header


class MeasureWithoutBuddyTests(NoBuddyConnectedTests):
    def test_not_connected(self):
        code, body, _ = self.request("POST", "/api/measure", {"mode": "stats"}, self.API)
        self.assertEqual(code, 503)
