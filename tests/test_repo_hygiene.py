"""Things that must never end up in the public repository: keys, private board files, internal names."""

import os
import re
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FORBIDDEN_CONTENT = [
    (re.compile(r"BEGIN [A-Z ]*PRIVATE KEY"), "a private key"),
    (re.compile(r"CONFIDENTIAL", re.I), "text marked confidential (the one place that says so is a doc rule, see below)"),
    (re.compile(r"\b(?:10|192\.168)\.\d+\.\d+(?:\.\d+)?\b"), "a private network address"),
    (re.compile(r"PCA10208|nrf54lm20dongle", re.I), "the unreleased nRF54LM20 Dongle (its board details are not public)"),
]
FORBIDDEN_NAMES = re.compile(r"(\.pem|\.hex|\.private\.overlay|keyfile\.json)$|(^|/)private/")

def extra_words():
    """Words you never want in the repository (host names, project code names), one per line, in a file outside it."""
    path = os.path.join(os.path.expanduser("~"), ".config", "claude-buddy", "hygiene-words.txt")
    try:
        with open(path, encoding="utf-8") as f:
            return [w.strip() for w in f if w.strip() and not w.startswith("#")]
    except OSError:
        return []


# Files that talk about the rules themselves.
ALLOWED = {"docs/DEVELOPMENT.md", ".gitignore", "tests/test_repo_hygiene.py"}


def tracked_files():
    out = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True)
    if out.returncode != 0:
        return None
    return out.stdout.splitlines()


class HygieneTests(unittest.TestCase):
    def setUp(self):
        self.files = tracked_files()
        if self.files is None:
            self.skipTest("not a git checkout")

    def test_no_private_file_names(self):
        bad = [f for f in self.files if FORBIDDEN_NAMES.search(f)]
        self.assertEqual(bad, [], "private or generated files are tracked")

    def test_no_secrets_or_internal_names_in_content(self):
        found = []
        for name in self.files:
            if name in ALLOWED:
                continue
            path = os.path.join(ROOT, name)
            try:
                text = open(path, encoding="utf-8").read()
            except (UnicodeDecodeError, OSError):
                continue  # binary or removed
            for pattern, what in FORBIDDEN_CONTENT + [(re.compile(re.escape(w), re.I), "a word from your hygiene-words.txt")
                                                      for w in extra_words()]:
                match = pattern.search(text)
                if match:
                    found.append(f"{name}: {what} ({match.group(0)!r})")
        self.assertEqual(found, [])


if __name__ == "__main__":
    unittest.main()
