"""The doctor, the MQTT login probe, and the interactive installer menu."""

import importlib.util
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INSTALL = os.path.join(ROOT, "install.py")
BRIDGE = os.path.join(ROOT, "bridge", "claude_buddy_bridge.py")


def load_emit():
    spec = importlib.util.spec_from_file_location("emit", os.path.join(ROOT, "hooks", "emit.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class FakeBroker:
    """Answers one MQTT CONNECT with the given CONNACK return code (or resets the connection)."""

    def __init__(self, return_code=None):
        self.return_code = return_code
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        self.port = self.server.getsockname()[1]
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        conn, _ = self.server.accept()
        conn.recv(1024)
        if self.return_code is None:
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
        else:
            conn.sendall(bytes([0x20, 0x02, 0x00, self.return_code]))
            conn.recv(16)
        conn.close()
        self.server.close()


class ProbeTests(unittest.TestCase):
    def probe(self, broker):
        return load_emit().probe_mqtt({"host": "127.0.0.1", "port": broker.port, "user": "u", "password": "p"})

    def test_successful_login(self):
        ok, msg = self.probe(FakeBroker(0))
        self.assertTrue(ok, msg)

    def test_bad_credentials_are_explained(self):
        ok, msg = self.probe(FakeBroker(5))
        self.assertFalse(ok)
        self.assertIn("not authorised", msg)

    def test_connection_reset_suggests_the_tunnel(self):
        ok, msg = self.probe(FakeBroker(None))
        self.assertFalse(ok)
        self.assertIn("tunnel", msg)

    def test_unreachable_host(self):
        ok, msg = load_emit().probe_mqtt({"host": "127.0.0.1", "port": free_port()})
        self.assertFalse(ok)
        self.assertIn("cannot reach", msg)


class DoctorTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.home = self.tmp.name
        self.env = dict(os.environ, HOME=self.home, USERPROFILE=self.home)
        self.port = free_port()
        subprocess.run([sys.executable, INSTALL, "--yes", "--claude", "yes", "--buddy", "no", "--relay", "join",
                        "--relay-host", "127.0.0.1", "--relay-password", "x"],
                       env=self.env, check=True, capture_output=True)
        path = os.path.join(self.home, ".config", "claude-buddy", "config.json")
        with open(path, encoding="utf-8") as f:
            cfg = json.load(f)
        cfg.update({"hook_transport": "http", "http_port": self.port})
        cfg["roles"]["relay"] = "none"
        cfg["bridge"]["inputs"] = ["http"]
        with open(path, "w", encoding="utf-8") as f:
            json.dump(cfg, f)
        self.config = path

    def tearDown(self):
        self.tmp.cleanup()

    def doctor(self):
        return subprocess.run([sys.executable, INSTALL, "--doctor"], env=self.env, capture_output=True, text=True)

    def test_reports_a_missing_bridge_and_then_a_healthy_one(self):
        result = self.doctor()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("nothing is listening", result.stdout)

        log = os.path.join(self.home, "bridge.log")
        proc = subprocess.Popen([sys.executable, BRIDGE, "--config", self.config, "--no-ble", "--log-file", log])
        try:
            for _ in range(100):
                if os.path.exists(log):
                    with open(log, encoding="utf-8") as f:
                        if "web page listening" in f.read():
                            break
                time.sleep(0.1)
            else:
                self.fail("the bridge did not start")
            result = self.doctor()
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("the bridge is listening", result.stdout)
        finally:
            proc.terminate()
            proc.wait(timeout=10)

    def test_without_a_config_it_says_to_run_the_installer(self):
        os.remove(self.config)
        result = self.doctor()
        self.assertEqual(result.returncode, 1)
        self.assertIn("run: python3 install.py", result.stdout)


@unittest.skipIf(sys.platform == "win32", "password prompt needs a console on Windows")
class MenuTests(unittest.TestCase):
    def test_choosing_option_3_with_an_existing_relay(self):
        with tempfile.TemporaryDirectory() as home:
            env = dict(os.environ, HOME=home, USERPROFILE=home)
            answers = "3\ny\nrelay.example.org\n\n\nsecret\nn\n\ny\n"
            result = subprocess.run([sys.executable, INSTALL, "--no-service"], input=answers, env=env,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("This is what I am going to do", result.stdout)
            with open(os.path.join(home, ".config", "claude-buddy", "config.json"), encoding="utf-8") as f:
                cfg = json.load(f)
            self.assertEqual(cfg["roles"], {"claude": True, "buddy": False, "relay": "join"})
            self.assertEqual(cfg["relay"]["host"], "relay.example.org")

    def test_declining_the_summary_changes_nothing(self):
        with tempfile.TemporaryDirectory() as home:
            env = dict(os.environ, HOME=home, USERPROFILE=home)
            result = subprocess.run([sys.executable, INSTALL, "--no-service"], input="1\n\nn\n", env=env,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Nothing was changed", result.stdout)
            self.assertFalse(os.path.exists(os.path.join(home, ".config", "claude-buddy")))


if __name__ == "__main__":
    unittest.main()


class StaleInstallTests(unittest.TestCase):
    def test_doctor_warns_when_installed_files_differ_from_the_checkout(self):
        with tempfile.TemporaryDirectory() as home:
            env = dict(os.environ, HOME=home, USERPROFILE=home)
            subprocess.run([sys.executable, INSTALL, "--yes", "--claude", "yes", "--buddy", "no", "--relay", "join",
                            "--relay-host", "127.0.0.1", "--relay-password", "x"], env=env, check=True,
                           capture_output=True)
            emit = os.path.join(home, ".local", "share", "claude-buddy", "emit.py")
            with open(emit, "a", encoding="utf-8") as f:
                f.write("\n# an older, different copy\n")
            result = subprocess.run([sys.executable, INSTALL, "--doctor"], env=env, capture_output=True, text=True)
            self.assertIn("installed emit.py differs from this checkout", result.stdout)
            self.assertNotIn("Traceback", result.stdout + result.stderr)

    def test_doctor_works_with_an_old_installed_emit_without_the_probe(self):
        with tempfile.TemporaryDirectory() as home:
            env = dict(os.environ, HOME=home, USERPROFILE=home)
            subprocess.run([sys.executable, INSTALL, "--yes", "--claude", "yes", "--buddy", "no", "--relay", "join",
                            "--relay-host", "127.0.0.1", "--relay-password", "x"], env=env, check=True,
                           capture_output=True)
            emit = os.path.join(home, ".local", "share", "claude-buddy", "emit.py")
            with open(emit, "w", encoding="utf-8") as f:
                f.write("# very old emit.py with no probe_mqtt\n")
            result = subprocess.run([sys.executable, INSTALL, "--doctor"], env=env, capture_output=True, text=True)
            self.assertNotIn("Traceback", result.stdout + result.stderr)
            self.assertIn("relay:", result.stdout)


class RunLimitedTests(unittest.TestCase):
    def load(self):
        spec = importlib.util.spec_from_file_location("install_rl", INSTALL)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_a_command_that_hangs_is_reported_as_timed_out(self):
        rc, _, _ = self.load().run_limited([sys.executable, "-c", "import time; time.sleep(30)"], seconds=1)
        self.assertIsNone(rc)

    def test_a_command_that_waits_for_input_does_not_hang(self):
        code = "import sys; sys.stdin.read(); print('done')"
        rc, out, _ = self.load().run_limited([sys.executable, "-c", code], seconds=10)
        self.assertEqual(rc, 0)
        self.assertIn("done", out)

    def test_a_missing_program_is_an_error_not_a_crash(self):
        rc, _, err = self.load().run_limited(["definitely-not-a-real-program-xyz"])
        self.assertEqual(rc, 127)
