#!/usr/bin/env python3
"""Claude Code status line command: forward the session's estimated cost to the buddy, then show your own status line.

Claude Code runs this on every status refresh and passes a JSON document on stdin (session id, cost, context window).
Whatever status line you had before is kept: it is stored as "statusline.chain" in ~/.config/claude-buddy/config.json
and run here with the same input; its output is printed unchanged. Never raises, always exits 0.

With "statusline.account" set, it also starts `statusline.py --credits` in the background at most every CREDITS_EVERY
seconds. That reads the month's spend and limit from your Claude account, the figure /usage shows under "Usage credits",
with the login Claude Code keeps in ~/.claude/.credentials.json, and forwards only the two amounts. The account endpoint
is not documented, so any surprise in its answer simply sends nothing and the bridge keeps using the estimate.
"""

import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emit  # noqa: E402  (the sibling file installed next to this one)

MIN_INTERVAL = 5  # seconds between sends for one session when nothing changed
CREDITS_EVERY = 300  # seconds between reads of the account's spend, across all sessions on this machine
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"  # what Claude Code's /usage screen reads


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


def credentials_path():
    base = os.environ.get("CLAUDE_CONFIG_DIR") or os.path.join(os.path.expanduser("~"), ".claude")
    return os.path.join(base, ".credentials.json")


def read_token(now=None):
    """The Claude login's access token, or None if there is none (API key, macOS keychain) or it has expired."""
    try:
        with open(credentials_path(), encoding="utf-8") as f:
            oauth = json.load(f).get("claudeAiOauth") or {}
    except (OSError, ValueError, AttributeError):
        return None
    token, expires = oauth.get("accessToken"), number(oauth.get("expiresAt"))  # expiresAt is in milliseconds
    if not isinstance(token, str) or not token or (expires is not None and expires / 1000 <= (now or time.time())):
        return None
    return token


def amount(money):
    """{"amount_minor": 18741, "exponent": 2, "currency": "USD"} -> 187.41, or None if it is anything else."""
    if not isinstance(money, dict) or money.get("currency", "USD") != "USD":
        return None
    minor, exponent = money.get("amount_minor"), money.get("exponent", 2)
    if not isinstance(minor, int) or isinstance(minor, bool) or not isinstance(exponent, int) or not 0 <= exponent <= 6:
        return None
    return minor / 10 ** exponent


def parse_credits(answer):
    """(used, limit) in US$ from the account's usage answer, or None if usage credits are off or the format changed."""
    spend = answer.get("spend") if isinstance(answer, dict) else None
    if not isinstance(spend, dict) or spend.get("enabled") is not True:
        return None
    used, limit = amount(spend.get("used")), amount(spend.get("limit"))
    if used is None or limit is None or not 0 <= used <= 1000000 or not 0 < limit <= 1000000:
        return None
    return used, limit


def credits_due():
    """True at most once per CREDITS_EVERY seconds on this machine; claims the slot so other sessions skip it."""
    stamp = os.path.join(tempfile.gettempdir(), "claude-buddy-credits")
    try:
        if time.time() - os.path.getmtime(stamp) < CREDITS_EVERY:
            return False
    except OSError:
        pass
    try:
        with open(stamp, "w", encoding="utf-8"):
            pass
    except OSError:
        return False
    return True


def start_credits_poll():
    """Runs the account read in its own process, so a slow network never holds up the status line."""
    flags = {"start_new_session": True} if os.name == "posix" else {"creationflags": 0x00000008 | 0x00000200}  # DETACHED, NEW_GROUP
    subprocess.Popen([sys.executable, os.path.abspath(__file__), "--credits"], stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True, **flags)


def poll_credits(cfg):
    token = read_token()
    if not token:
        return
    req = urllib.request.Request(USAGE_URL, headers={"Authorization": f"Bearer {token}", "User-Agent": "claude-buddy"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        found = parse_credits(json.load(resp))
    if found:
        emit.send_credits(cfg, *found)


def main():
    if sys.argv[1:] == ["--credits"]:
        try:
            poll_credits(emit.load_config())
        except Exception:  # noqa: BLE001 - offline, logged out, format changed: the estimate stays in use
            pass
        return
    try:
        raw = sys.stdin.read()
    except Exception:  # noqa: BLE001
        raw = ""
    cfg = emit.load_config()
    try:
        forward(raw, cfg)
    except Exception:  # noqa: BLE001 - never disturb Claude Code
        pass
    try:
        if (cfg.get("statusline") or {}).get("account") and credits_due():
            start_credits_poll()
    except Exception:  # noqa: BLE001
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
