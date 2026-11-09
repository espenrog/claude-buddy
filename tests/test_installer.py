"""The installer against a throwaway home directory (relay-client role: no network, no Bluetooth)."""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from unittest.mock import patch

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INSTALL = os.path.join(ROOT, "install.py")
ARGS = ["--yes", "--claude", "yes", "--buddy", "no", "--relay", "join", "--relay-host", "relay.example.org",
        "--relay-user", "buddy"]  # the password comes from the environment (see setUp)


def load_installer(home):
    """Imports install.py as a module, with the home directory pointed at `home`."""
    import importlib.util
    saved = {k: os.environ.get(k) for k in ("HOME", "USERPROFILE", "COPILOT_HOME")}
    os.environ["HOME"] = os.environ["USERPROFILE"] = home
    os.environ.pop("COPILOT_HOME", None)
    try:
        spec = importlib.util.spec_from_file_location("install_under_test", INSTALL)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.home = self.tmp.name
        self.env = dict(os.environ, HOME=self.home, USERPROFILE=self.home, CLAUDE_BUDDY_RELAY_PASSWORD="not-a-real-password")
        self.env.pop("COPILOT_HOME", None)
        os.makedirs(os.path.join(self.home, ".claude"))
        self.settings = os.path.join(self.home, ".claude", "settings.json")
        with open(self.settings, "w", encoding="utf-8") as f:
            json.dump({"model": "x", "hooks": {"Stop": [{"matcher": "", "hooks": [
                {"type": "command", "command": "echo my-own-hook"}]}]}}, f)

    def tearDown(self):
        self.tmp.cleanup()

    def run_install(self, *args):
        result = subprocess.run([sys.executable, INSTALL, *args], env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def load(self):
        with open(self.settings, encoding="utf-8") as f:
            return json.load(f)

    def ours(self):
        return [h for groups in self.load().get("hooks", {}).values() for g in groups for h in g["hooks"]
                if "claude-buddy" in h["command"]]

    def test_install_adds_twelve_hooks_and_keeps_user_settings(self):
        self.run_install(*ARGS)
        self.assertEqual(len(self.ours()), 12)
        settings = self.load()
        self.assertEqual(settings["model"], "x")
        stop_cmds = [h["command"] for g in settings["hooks"]["Stop"] for h in g["hooks"]]
        self.assertIn("echo my-own-hook", stop_cmds)

    def test_spending_is_opt_in_and_keeps_the_users_status_line(self):
        self.run_install(*ARGS)
        self.assertNotIn("statusLine", self.load())
        settings = self.load()
        settings["statusLine"] = {"type": "command", "command": "echo my-own-line"}
        with open(self.settings, "w", encoding="utf-8") as f:
            json.dump(settings, f)
        self.run_install(*ARGS, "--spending", "yes")
        self.run_install(*ARGS)  # a later run without the flag keeps the choice
        line = self.load()["statusLine"]
        self.assertIn("statusline.py", line["command"])
        with open(os.path.join(self.home, ".config", "claude-buddy", "config.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f)["statusline"], {"enabled": True, "chain": "echo my-own-line"})
        self.run_install(*ARGS, "--spending", "no")
        self.assertEqual(self.load()["statusLine"]["command"], "echo my-own-line")

    def test_uninstall_restores_the_status_line(self):
        settings = self.load()
        settings["statusLine"] = {"type": "command", "command": "echo my-own-line"}
        with open(self.settings, "w", encoding="utf-8") as f:
            json.dump(settings, f)
        self.run_install(*ARGS, "--spending", "yes")
        self.run_install("--uninstall")
        self.assertEqual(self.load()["statusLine"]["command"], "echo my-own-line")

    def test_install_is_idempotent(self):
        self.run_install(*ARGS)
        self.run_install(*ARGS)
        self.assertEqual(len(self.ours()), 12)

    def test_config_has_the_relay_and_is_private(self):
        self.run_install(*ARGS)
        path = os.path.join(self.home, ".config", "claude-buddy", "config.json")
        with open(path, encoding="utf-8") as f:
            cfg = json.load(f)
        self.assertEqual(cfg["hook_transport"], "mqtt")
        self.assertEqual(cfg["relay"]["host"], "relay.example.org")
        if sys.platform != "win32":
            self.assertEqual(os.stat(path).st_mode & 0o777, 0o600)

    def test_a_backup_of_settings_is_made(self):
        self.run_install(*ARGS)
        backups = [n for n in os.listdir(os.path.join(self.home, ".claude")) if n.endswith(".bak")]
        self.assertEqual(len(backups), 1)

    def test_uninstall_removes_only_our_hooks(self):
        self.run_install(*ARGS)
        self.run_install("--uninstall")
        self.assertEqual(self.ours(), [])
        settings = self.load()
        self.assertEqual(settings["model"], "x")
        self.assertEqual(settings["hooks"]["Stop"][0]["hooks"][0]["command"], "echo my-own-hook")

    def test_invalid_settings_json_is_not_touched(self):
        with open(self.settings, "w", encoding="utf-8") as f:
            f.write("{ this is not json")
        result = subprocess.run([sys.executable, INSTALL, *ARGS], env=self.env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        with open(self.settings, encoding="utf-8") as f:
            self.assertEqual(f.read(), "{ this is not json")

    def test_hook_command_actually_runs(self):
        self.run_install(*ARGS)
        cmd = next(h["command"] for h in self.ours() if h["command"].endswith("turn_finished"))
        result = subprocess.run(cmd, shell=True, env=self.env, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)  # relay is unreachable: must still exit 0

    def copilot_path(self):
        return Path(self.home) / ".copilot" / "hooks" / "claude-buddy.json"

    def copilot_hooks(self):
        return json.loads(self.copilot_path().read_text(encoding="utf-8"))["hooks"]

    def test_copilot_is_opt_in(self):
        self.run_install(*ARGS)
        self.assertFalse(self.copilot_path().exists())

    def test_copilot_install_is_idempotent_and_remembers_the_choice(self):
        self.run_install(*ARGS, "--copilot", "yes")
        self.run_install(*ARGS)
        hooks = self.copilot_hooks()
        self.assertEqual(set(hooks), {"SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse",
                                     "PostToolUseFailure", "Stop", "SessionEnd"})
        self.assertTrue(all(len(entries) == 1 for entries in hooks.values()))
        self.assertTrue(all(entries[0]["timeoutSec"] == 5 for entries in hooks.values()))
        self.assertIn("7 Claude Buddy hooks", self.run_install("--status"))
        backups = list(self.copilot_path().parent.glob("*.bak"))
        self.assertTrue(backups)

    def test_copilot_only_relay_install_does_not_change_claude_settings(self):
        before = Path(self.settings).read_bytes()
        self.run_install(*ARGS, "--claude", "no", "--copilot", "yes")
        self.assertEqual(Path(self.settings).read_bytes(), before)
        cfg = json.loads((Path(self.home) / ".config/claude-buddy/config.json").read_text(encoding="utf-8"))
        self.assertEqual(cfg["roles"], {"claude": False, "copilot": True, "buddy": False, "relay": "join"})
        self.assertEqual(cfg["hook_transport"], "mqtt")
        self.assertNotIn("statusLine", self.load())

    def test_copilot_home_override_is_used(self):
        self.env["COPILOT_HOME"] = str(Path(self.home) / "custom copilot")
        self.run_install(*ARGS, "--copilot", "yes")
        custom = Path(self.env["COPILOT_HOME"]) / "hooks/claude-buddy.json"
        self.assertTrue(custom.exists())
        self.assertFalse(self.copilot_path().exists())
        self.run_install("--uninstall")
        self.assertFalse(custom.exists())

    def test_copilot_opt_out_and_uninstall_preserve_other_hooks(self):
        for action in (("--copilot", "no"), ("--uninstall",)):
            with self.subTest(action=action):
                path = self.copilot_path()
                path.parent.mkdir(parents=True, exist_ok=True)
                other = path.with_name("my-hooks.json")
                other.write_text('{"version":1,"hooks":{}}\n', encoding="utf-8")
                original = {"version": 1, "hooks": {
                    "Stop": [{"type": "command", "command": "echo user-hook"}]}}
                path.write_text(json.dumps(original), encoding="utf-8")
                self.run_install(*ARGS, "--copilot", "yes")
                self.run_install(*(action if action == ("--uninstall",) else (*ARGS, *action)))
                self.assertEqual(json.loads(path.read_text(encoding="utf-8")), original)
                self.assertEqual(other.read_text(encoding="utf-8"), '{"version":1,"hooks":{}}\n')

    def test_uninstall_removes_owned_copilot_file(self):
        self.run_install(*ARGS, "--copilot", "yes")
        self.run_install("--uninstall")
        self.assertFalse(self.copilot_path().exists())

    def test_invalid_copilot_file_does_not_change_either_agents_settings(self):
        path = self.copilot_path()
        path.parent.mkdir(parents=True)
        for content in ("{ invalid", "[]", '{"version":1,"hooks":{"Stop":[null]}}',
                        '{"version":2,"hooks":{}}'):
            with self.subTest(content=content):
                path.write_text(content, encoding="utf-8")
                before = Path(self.settings).read_bytes()
                result = subprocess.run([sys.executable, INSTALL, *ARGS, "--copilot", "yes"],
                                        env=self.env, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertNotIn("Traceback", result.stderr)
                self.assertEqual(path.read_text(encoding="utf-8"), content)
                self.assertEqual(Path(self.settings).read_bytes(), before)

    def test_generated_copilot_commands_forward_events_and_session_ids(self):
        self.run_install(*ARGS, "--copilot", "yes")
        paths = []

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                paths.append(self.path)
                self.send_response(200)
                self.end_headers()

            def log_message(self, *args):
                pass

        server = HTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        config = Path(self.home) / ".config/claude-buddy/config.json"
        config.write_text(json.dumps({"hook_transport": "http", "http_port": server.server_port}), encoding="utf-8")
        expected = {"SessionStart": "session_started", "UserPromptSubmit": "prompt_submitted",
                    "PreToolUse": "answered", "PostToolUse": "answered",
                    "PostToolUseFailure": "error", "Stop": "turn_finished", "SessionEnd": "session_ended"}
        try:
            for name, entries in self.copilot_hooks().items():
                for sid in ("copilot-session-one", "copilot-session-two"):
                    entry = entries[0]
                    command = (["powershell", "-NoProfile", "-Command", entry["powershell"]]
                               if sys.platform == "win32" else entry["command"])
                    result = subprocess.run(command, shell=sys.platform != "win32", env=self.env,
                                            input=json.dumps({"hook_event_name": name, "session_id": sid}),
                                            capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout + result.stderr, "")
                    self.assertEqual(paths[-1], f"/e/{expected[name]}/{sid}")
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def test_copilot_only_local_setup_needs_no_relay(self):
        inst = load_installer(self.home)
        before = Path(self.settings).read_bytes()
        with patch.object(sys, "argv", [INSTALL, "--yes", "--claude", "no", "--copilot", "yes", "--no-service"]), \
                patch.object(inst, "setup_venv") as setup_venv, patch.object(inst, "setup_broker") as setup_broker:
            inst.main()
        cfg = json.loads(inst.CONFIG.read_text(encoding="utf-8"))
        self.assertEqual(cfg["roles"], {"claude": False, "copilot": True, "buddy": True, "relay": "none"})
        self.assertEqual(cfg["hook_transport"], "http")
        self.assertEqual(cfg["bridge"]["inputs"], ["http"])
        self.assertTrue(self.copilot_path().exists())
        self.assertEqual(Path(self.settings).read_bytes(), before)
        setup_venv.assert_called_once_with()
        setup_broker.assert_not_called()

    def test_powershell_command_quotes_paths_with_spaces_and_apostrophes(self):
        inst = load_installer(self.home)
        with patch.object(inst, "DATA_DIR", Path(self.home) / "user's buddy files"):
            hook = inst.copilot_hook("turn_finished")
        self.assertIn("user''s buddy files/emit.py' turn_finished", hook["powershell"])
        self.assertTrue(hook["powershell"].startswith("& '"))


if __name__ == "__main__":
    unittest.main()


class RelayPasswordReuseTests(unittest.TestCase):
    """Re-running the installer on a relay host must not change the password (that would lock out bridges)."""

    def write_config(self, home, cfg):
        path = os.path.join(home, ".config", "claude-buddy")
        os.makedirs(path)
        with open(os.path.join(path, "config.json"), "w", encoding="utf-8") as f:
            json.dump(cfg, f)

    def test_password_is_reused_on_a_relay_host(self):
        with tempfile.TemporaryDirectory() as home:
            self.write_config(home, {"roles": {"relay": "host"}, "relay": {"password": "keep-me"}})
            self.assertEqual(load_installer(home).existing_relay_password(), "keep-me")

    def test_older_config_hosting_on_localhost_counts_as_a_host(self):
        with tempfile.TemporaryDirectory() as home:
            self.write_config(home, {"relay": {"host": "localhost", "password": "old-style"}})
            self.assertEqual(load_installer(home).existing_relay_password(), "old-style")

    def test_a_relay_client_does_not_reuse_anything(self):
        with tempfile.TemporaryDirectory() as home:
            self.write_config(home, {"roles": {"relay": "join"}, "relay": {"password": "someone-elses"}})
            self.assertIsNone(load_installer(home).existing_relay_password())

    def test_no_config_means_no_password(self):
        with tempfile.TemporaryDirectory() as home:
            self.assertIsNone(load_installer(home).existing_relay_password())

class RelayPasswordSecrecyTests(unittest.TestCase):
    """The relay password must not show up in a command line (shell history, `ps`) or reach the broker in clear."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.inst = load_installer(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def args(self, file=None):
        return argparse.Namespace(relay_password_file=file)

    def test_the_password_file_line_is_mosquittos_pbkdf2_format(self):
        import base64
        import hashlib
        line = self.inst.mosquitto_password_line("claude-buddy", "s3cret", salt=bytes(range(12)))
        user, rest = line.split(":", 1)
        _, kind, rounds, salt, digest = rest.split("$")
        self.assertEqual((user, kind, rounds), ("claude-buddy", "7", "101"))
        self.assertEqual(base64.b64decode(salt), bytes(range(12)))
        self.assertEqual(base64.b64decode(digest), hashlib.pbkdf2_hmac("sha512", b"s3cret", bytes(range(12)), 101))
        self.assertNotIn("s3cret", line)
        other = self.inst.mosquitto_password_line("claude-buddy", "s3cret")
        self.assertNotEqual(other, self.inst.mosquitto_password_line("claude-buddy", "s3cret"))  # a new salt each time

    def test_unattended_password_from_a_file_stdin_or_the_environment(self):
        path = os.path.join(self.tmp.name, "pw")
        with open(path, "w", encoding="utf-8") as f:
            f.write("from-file\n")
        saved = os.environ.pop(self.inst.RELAY_PASSWORD_ENV, None)
        try:
            self.assertEqual(self.inst.preset_relay_password(self.args(path)), "from-file")
            self.assertIsNone(self.inst.preset_relay_password(self.args()))
            os.environ[self.inst.RELAY_PASSWORD_ENV] = "from-env"
            self.assertEqual(self.inst.preset_relay_password(self.args()), "from-env")
            self.assertEqual(self.inst.preset_relay_password(self.args(path)), "from-file")  # the file wins
        finally:
            os.environ.pop(self.inst.RELAY_PASSWORD_ENV, None)
            if saved is not None:
                os.environ[self.inst.RELAY_PASSWORD_ENV] = saved
        result = subprocess.run([sys.executable, "-c", "import importlib.util, argparse, sys;"
                                 f"s = importlib.util.spec_from_file_location('i', {INSTALL!r}); m = importlib.util.module_from_spec(s);"
                                 "s.loader.exec_module(m); print(m.preset_relay_password(argparse.Namespace(relay_password_file='-')))"],
                                input="from-stdin\n", capture_output=True, text=True, env=dict(os.environ, HOME=self.tmp.name))
        self.assertEqual(result.stdout.strip(), "from-stdin", result.stderr)

    def test_an_empty_password_file_is_refused(self):
        path = os.path.join(self.tmp.name, "empty")
        open(path, "w").close()
        with self.assertRaises(SystemExit):
            self.inst.preset_relay_password(self.args(path))

    def test_there_is_no_plain_password_option(self):
        env = dict(os.environ, HOME=self.tmp.name, USERPROFILE=self.tmp.name,
                   COPILOT_HOME=os.path.join(self.tmp.name, ".copilot"))
        result = subprocess.run([sys.executable, INSTALL, "--relay-password", "leaky-value", "--yes"], capture_output=True,
                                text=True, env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("leaky-value", result.stdout + result.stderr)
        # An abbreviation must not be taken for --relay-password-file (that would try to open a file named like the password).
        result = subprocess.run([sys.executable, INSTALL, "--relay-pass", "leaky-value", "--yes"], capture_output=True,
                                text=True, env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("password file", result.stdout + result.stderr)

    def test_setting_up_the_broker_never_puts_the_password_on_a_command_line(self):
        calls = []

        def fake_run(cmd, **kw):
            calls.append((cmd, kw.get("input")))
        self.inst.run, self.inst.have = fake_run, lambda _: True
        self.inst.setup_broker(1883, "claude-buddy", "s3cret-value")
        self.assertTrue(calls)
        for cmd, data in calls:
            self.assertFalse(any("s3cret-value" in str(c) for c in cmd), cmd)
            self.assertNotIn(b"s3cret-value", data or b"")
        self.assertTrue(any(data and b"claude-buddy:$7$101$" in data for _, data in calls))
