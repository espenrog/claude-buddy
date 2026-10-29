#!/usr/bin/env python3
"""Claude Code status line command: forward the session's estimated cost to the buddy, then show your own status line.

Claude Code runs this on every status refresh and passes a JSON document on stdin (session id, cost, context window).
Whatever status line you had before is kept: it is stored as "statusline.chain" in ~/.config/claude-buddy/config.json
and run here with the same input; its output is printed unchanged. Never raises, always exits 0.
"""

import json
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emit  # noqa: E402  (the sibling file installed next to this one)

MIN_INTERVAL = 5  # seconds between sends for one session when nothing changed


def number(value):
    return value if isinstance(value, (int, float)) and not isinstance(value, bool) else None


def forward(raw, cfg):
    data = json.loads(raw)
    session = data.get("session_id")
    cost = number((data.get("cost") or {}).get("total_cost_usd"))
    if not isinstance(session, str) or not emit.SESSION_ID.match(session) or cost is None:
        return
    window = data.get("context_window") or {}
    pct = number(window.get("used_percentage"))
    stamp = os.path.join(tempfile.gettempdir(), f"claude-buddy-usage-{session}")
    state = f"{cost:g} {pct}"
    try:
        with open(stamp, encoding="utf-8") as f:
            last_state, _, last_time = f.read().partition("|")
        if last_state == state and time.time() - float(last_time) < MIN_INTERVAL:
            return
    except (OSError, ValueError):
        pass
    emit.send_usage(cfg, session, cost, pct)
    try:
        with open(stamp, "w", encoding="utf-8") as f:
            f.write(f"{state}|{time.time()}")
    except OSError:
        pass


def main():
    try:
        raw = sys.stdin.read()
    except Exception:  # noqa: BLE001
        raw = ""
    cfg = emit.load_config()
    try:
        forward(raw, cfg)
    except Exception:  # noqa: BLE001 - never disturb Claude Code
        pass
    chain = (cfg.get("statusline") or {}).get("chain")
    if chain:
        try:
            out = subprocess.run(chain, shell=True, input=raw, capture_output=True, text=True, timeout=3).stdout
            sys.stdout.write(out)
        except Exception:  # noqa: BLE001
            pass


if __name__ == "__main__":
    main()
