"""Bridge + hook emitter, end to end over HTTP, without Bluetooth."""

import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BRIDGE = os.path.join(ROOT, "bridge", "claude_buddy_bridge.py")
EMIT = os.path.join(ROOT, "hooks", "emit.py")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for(path, text, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                if text in f.read():
                    return True
        time.sleep(0.1)
    return False


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.port = free_port()
        self.config = os.path.join(self.tmp.name, "config.json")
        with open(self.config, "w", encoding="utf-8") as f:
            json.dump({"hook_transport": "http", "http_port": self.port,
                       "bridge": {"inputs": ["http"], "ble": False}}, f)
        self.log = os.path.join(self.tmp.name, "bridge.log")
        self.proc = subprocess.Popen(
            [sys.executable, BRIDGE, "--config", self.config, "--no-ble", "--log-file", self.log])
        self.assertTrue(wait_for(self.log, "web page listening"), "bridge did not start")
        self.env = dict(os.environ, CLAUDE_BUDDY_CONFIG=self.config)

    def tearDown(self):
        self.proc.terminate()
        self.proc.wait(timeout=10)
        self.tmp.cleanup()

    def emit(self, event):
        return subprocess.run([sys.executable, EMIT, event], env=self.env, capture_output=True, text=True,
                              stdin=subprocess.DEVNULL)

    def test_events_reach_the_bridge_with_the_right_codes(self):
        for event, code in [("prompt_submitted", "0x03"), ("needs_input", "0x05"), ("turn_finished", "0x04")]:
            self.assertEqual(self.emit(event).returncode, 0)
            self.assertTrue(wait_for(self.log, f"event {event} -> {code}"), event)

    def test_unknown_event_is_rejected(self):
        self.emit("bogus_event")
        self.assertTrue(wait_for(self.log, "Ignoring unknown event 'bogus_event'"))

    def test_bad_requests_get_proper_status_codes(self):
        import urllib.error
        import urllib.request
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}/e/idle", method="GET")
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            urllib.request.urlopen(req, timeout=3)
        self.assertEqual(ctx.exception.code, 404)
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}/e/nonsense", data=b"", method="POST")
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            urllib.request.urlopen(req, timeout=3)
        self.assertEqual(ctx.exception.code, 400)


class EmitWithoutBridgeTests(unittest.TestCase):
    def test_emit_is_silent_and_exits_zero_when_nothing_listens(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = os.path.join(tmp, "config.json")
            with open(config, "w", encoding="utf-8") as f:
                json.dump({"hook_transport": "http", "http_port": free_port()}, f)
            result = subprocess.run([sys.executable, EMIT, "turn_finished"],
                                    env=dict(os.environ, CLAUDE_BUDDY_CONFIG=config),
                                    capture_output=True, text=True, timeout=15, stdin=subprocess.DEVNULL)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stdout + result.stderr, "")

    def test_emit_survives_a_missing_config(self):
        result = subprocess.run([sys.executable, EMIT, "idle"],
                                env=dict(os.environ, CLAUDE_BUDDY_CONFIG="/nonexistent/config.json"),
                                capture_output=True, text=True, timeout=15, stdin=subprocess.DEVNULL)
        self.assertEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()


class FirmwareVersionTests(unittest.TestCase):
    def setUp(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("bridge_under_test", BRIDGE)
        self.bridge = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.bridge)

    def test_matching_protocol_is_fine(self):
        ok, msg = self.bridge.describe_firmware(bytes([self.bridge.PROTOCOL_VERSION, 0, 2]))
        self.assertTrue(ok)
        self.assertIn("firmware 0.2", msg)

    def test_patch_byte_gives_full_version(self):
        proto = self.bridge.PROTOCOL_VERSION
        self.assertEqual(self.bridge.firmware_version(bytes([proto, 0, 8])), "0.8")
        self.assertEqual(self.bridge.firmware_version(bytes([proto, 0, 8, 1])), "0.8")
        self.assertEqual(self.bridge.firmware_version(bytes([proto, 0, 8, 1, 3])), "0.8.3")
        ok, msg = self.bridge.describe_firmware(bytes([proto, 0, 8, 1, 3]))
        self.assertTrue(ok)
        self.assertIn("firmware 0.8.3,", msg)

    def test_missing_characteristic_means_old_firmware(self):
        ok, msg = self.bridge.describe_firmware(None)
        self.assertFalse(ok)
        self.assertIn("older than 0.2", msg)

    def test_truncated_data_is_treated_as_unknown(self):
        self.assertFalse(self.bridge.describe_firmware(b"\x01")[0])

    def test_newer_firmware_asks_to_update_the_bridge(self):
        ok, msg = self.bridge.describe_firmware(bytes([self.bridge.PROTOCOL_VERSION + 1, 1, 0]))
        self.assertFalse(ok)
        self.assertIn("update the bridge", msg.replace("re-run install.py to ", ""))

    def test_older_firmware_asks_to_update_the_firmware(self):
        ok, msg = self.bridge.describe_firmware(bytes([0, 0, 1]))
        self.assertFalse(ok)
        self.assertIn("update the buddy's firmware", msg)


def load_module(path, name):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class SessionSlotTests(unittest.TestCase):
    def setUp(self):
        self.bridge = load_module(BRIDGE, "bridge_sessions")

    def test_each_session_gets_its_own_slot_and_keeps_it(self):
        slots = self.bridge.SessionSlots(size=4)
        self.assertEqual(slots.slot_for("a"), (0, False))
        self.assertEqual(slots.slot_for("b"), (1, False))
        self.assertEqual(slots.slot_for("a"), (0, False))

    def test_ending_a_session_frees_its_slot(self):
        slots = self.bridge.SessionSlots(size=4)
        slots.slot_for("a")
        slots.slot_for("b")
        slots.end("a")
        self.assertEqual(slots.slot_for("c"), (0, False))

    def test_the_least_recently_used_slot_is_recycled_when_full(self):
        slots = self.bridge.SessionSlots(size=2)
        slots.slot_for("a")
        slots.slot_for("b")
        slots.slot_for("a")  # b is now the least recently used
        self.assertEqual(slots.slot_for("c"), (1, True))
        self.assertEqual(slots.slot_for("a"), (0, False))

    def test_parse_event(self):
        self.assertEqual(self.bridge.parse_event("turn_finished"), ("turn_finished", None))
        self.assertEqual(self.bridge.parse_event("turn_finished:abc-123_X"), ("turn_finished", "abc-123_X"))
        self.assertEqual(self.bridge.parse_event("idle:bad id!"), ("idle", None))
        self.assertEqual(self.bridge.parse_event(" idle \n"), ("idle", None))

    def test_a_recycled_slot_is_reset_before_its_first_event(self):
        import queue as _queue
        q = _queue.Queue()
        slots = self.bridge.SessionSlots(size=1)
        self.bridge.submit("prompt_submitted:one", q, slots)
        self.bridge.submit("prompt_submitted:two", q, slots)
        items = [q.get_nowait() for _ in range(3)]
        self.assertEqual(items, [(0x03, 0), (0x01, 0), (0x03, 0)])  # ... reset (session_started) in between

    def test_session_ended_frees_the_slot_for_the_next_session(self):
        import queue as _queue
        q = _queue.Queue()
        slots = self.bridge.SessionSlots(size=1)
        self.bridge.submit("prompt_submitted:one", q, slots)
        self.bridge.submit("session_ended:one", q, slots)
        self.bridge.submit("prompt_submitted:two", q, slots)
        self.assertEqual([q.get_nowait() for _ in range(3)], [(0x03, 0), (0x02, 0), (0x03, 0)])  # no extra reset

    def test_firmware_protocol_1_still_works(self):
        ok, msg = self.bridge.describe_firmware(bytes([1, 0, 2]))
        self.assertTrue(ok)
        self.assertIn("firmware 0.3", msg)


class EmitSessionTests(BridgeTests):
    """Runs against a live bridge; emit.py reads the session id from the hook's stdin."""

    def emit_with_stdin(self, event, text):
        return subprocess.run([sys.executable, EMIT, event], env=self.env, input=text, capture_output=True,
                              text=True, timeout=15)

    def test_session_id_from_hook_input_selects_the_slot(self):
        self.emit_with_stdin("prompt_submitted", json.dumps({"session_id": "sess-A", "cwd": "/x"}))
        self.assertTrue(wait_for(self.log, "event prompt_submitted -> 0x03 (slot 0)"))
        self.emit_with_stdin("prompt_submitted", json.dumps({"session_id": "sess-B"}))
        self.assertTrue(wait_for(self.log, "event prompt_submitted -> 0x03 (slot 1)"))
        self.emit_with_stdin("turn_finished", json.dumps({"session_id": "sess-A"}))
        self.assertTrue(wait_for(self.log, "event turn_finished -> 0x04 (slot 0)"))

    def test_garbage_on_stdin_is_ignored(self):
        for text in ("not json", json.dumps({"session_id": "bad id!"}), json.dumps({"no": "session"}), ""):
            self.assertEqual(self.emit_with_stdin("idle", text).returncode, 0)
        self.assertTrue(wait_for(self.log, "event idle -> 0x09 (slot 0)"))

    def test_an_input_pipe_that_never_closes_does_not_hang_emit(self):
        proc = subprocess.Popen([sys.executable, EMIT, "idle"], env=self.env, stdin=subprocess.PIPE,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            self.assertEqual(proc.wait(timeout=5), 0)  # gives up waiting after about half a second
        finally:
            proc.stdin.close()


class MqttPayloadTests(unittest.TestCase):
    def test_session_is_appended_to_the_event_name(self):
        import threading
        emit = load_module(EMIT, "emit_payload")
        server = socket.socket()
        server.bind(("127.0.0.1", 0))
        server.listen(1)
        received = []

        def serve():
            conn, _ = server.accept()
            conn.recv(1024)  # CONNECT
            conn.sendall(bytes([0x20, 0x02, 0x00, 0x00]))
            conn.settimeout(2)
            data = b""
            try:
                while True:
                    chunk = conn.recv(1024)
                    if not chunk:
                        break
                    data += chunk
            except socket.timeout:
                pass
            received.append(data)
            conn.close()

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        emit.send_mqtt({"host": "127.0.0.1", "port": server.getsockname()[1], "topic": "t/x"}, "needs_input", "sess-1")
        thread.join(5)
        self.assertIn(b"t/xneeds_input:sess-1", received[0])
        server.close()
