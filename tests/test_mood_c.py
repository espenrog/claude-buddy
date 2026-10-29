"""Builds and runs the C unit tests (mood state machine, shake detector, settings format); skipped if no C compiler."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class CTests(unittest.TestCase):
    def build_and_run(self, source, test, expected):
        cc = next((c for c in ("cc", "gcc", "clang") if shutil.which(c)), None)
        if cc is None:
            self.skipTest("no C compiler found")
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "t.exe" if sys.platform == "win32" else "t")
            subprocess.run(
                [cc, "-Wall", "-Wextra", "-I", os.path.join(ROOT, "firmware", "src"),
                 os.path.join(ROOT, "firmware", "src", source),
                 os.path.join(ROOT, "tests", test), "-o", exe],
                check=True,
            )
            result = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(expected, result.stdout)

    def test_mood_state_machine(self):
        self.build_and_run("mood.c", "test_mood.c", "all mood tests passed")

    def test_shake_detector(self):
        self.build_and_run("shake.c", "test_shake.c", "all shake tests passed")

    def test_settings_wire_format(self):
        self.build_and_run("buddy_cfg.c", "test_cfg.c", "all cfg tests passed")

    def test_measure_mode(self):
        self.build_and_run("measure_mode.c", "test_measure_mode.c", "all measure mode tests passed")

    def test_battery_log(self):
        self.build_and_run("battery_log.c", "test_battery_log.c", "all battery log tests passed")


if __name__ == "__main__":
    unittest.main()
