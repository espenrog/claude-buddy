#!/usr/bin/env python3
"""Claude Code hook entry point: send one event name to the buddy pipeline.

Usage: emit.py <event>      (e.g. emit.py turn_finished)

Reads ~/.config/claude-buddy/config.json and sends the event either to the local bridge
(HTTP) or to an MQTT relay. Standard library only, so it works on any machine with Python 3.
It never raises and always exits 0: a missing buddy must never get in Claude's way.
"""

import json
import os
import re
import socket
import sys
import threading
import urllib.request

CONFIG = os.environ.get(
    "CLAUDE_BUDDY_CONFIG",
    os.path.join(os.path.expanduser("~"), ".config", "claude-buddy", "config.json"),
)


def load_config():
    try:
        with open(CONFIG, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


SESSION_ID = re.compile(r"^[A-Za-z0-9_-]{1,64}$")


def read_session_id(wait=0.5):
    """Claude Code passes a JSON document on stdin; returns its session_id, or None.
    Never blocks for longer than `wait` seconds (stdin may be an open pipe that never closes)."""
    try:
        if sys.stdin is None or sys.stdin.isatty():
            return None
    except Exception:  # noqa: BLE001
        return None
    box = []

    def reader():
        try:
            box.append(sys.stdin.read())
        except Exception:  # noqa: BLE001
            pass

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    thread.join(wait)
    if not box:
        return None
    try:
        sid = json.loads(box[0]).get("session_id")
    except (ValueError, AttributeError):
        return None
    return sid if isinstance(sid, str) and SESSION_ID.match(sid) else None


def send_http(port, event, session=None):
    path = f"/e/{event}" + (f"/{session}" if session else "")
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=b"", method="POST")
    urllib.request.urlopen(req, timeout=1.5).close()


def _encode_str(text):
    raw = text.encode("utf-8")
    return len(raw).to_bytes(2, "big") + raw


def _remaining_length(n):
    out = bytearray()
    while True:
        digit, n = n % 128, n // 128
        out.append(digit | (0x80 if n else 0))
        if not n:
            return bytes(out)


def send_mqtt(cfg, event, session=None):
    """Minimal MQTT 3.1.1 client: CONNECT, PUBLISH (QoS 0), DISCONNECT."""
    host = cfg.get("host", "localhost")
    port = int(cfg.get("port", 1883))
    user, password = cfg.get("user"), cfg.get("password", "")
    topic = cfg.get("topic", "claude-buddy/events")

    flags = 0x02  # clean session
    payload = _encode_str(f"claude-buddy-emit-{os.getpid()}")
    if user:
        flags |= 0xC0
        payload += _encode_str(user) + _encode_str(password)
    variable = _encode_str("MQTT") + bytes([4, flags]) + (10).to_bytes(2, "big")
    body = variable + payload
    connect = b"\x10" + _remaining_length(len(body)) + body

    text = event + (f":{session}" if session else "")
    pub_body = _encode_str(topic) + text.encode("utf-8")
    publish = b"\x30" + _remaining_length(len(pub_body)) + pub_body

    with socket.create_connection((host, port), timeout=2) as sock:
        sock.settimeout(2)
        sock.sendall(connect)
        connack = sock.recv(4)
        if len(connack) < 4 or connack[0] != 0x20 or connack[3] != 0:
            raise OSError(f"MQTT connection refused: {connack!r}")
        sock.sendall(publish + b"\xe0\x00")


def probe_mqtt(cfg):
    """Connects and logs in only (publishes nothing). Returns (ok, human-readable result)."""
    host, port = cfg.get("host", "localhost"), int(cfg.get("port", 1883))
    user, password = cfg.get("user"), cfg.get("password", "")
    flags = 0x02
    payload = _encode_str(f"claude-buddy-probe-{os.getpid()}")
    if user:
        flags |= 0xC0
        payload += _encode_str(user) + _encode_str(password)
    body = _encode_str("MQTT") + bytes([4, flags]) + (10).to_bytes(2, "big") + payload
    try:
        with socket.create_connection((host, port), timeout=3) as sock:
            sock.settimeout(3)
            sock.sendall(b"\x10" + _remaining_length(len(body)) + body)
            connack = sock.recv(4)
            if len(connack) < 4:
                return False, "connected, but the server closed the connection before answering"
            if connack[3] == 0:
                sock.sendall(b"\xe0\x00")
                return True, f"login to {host}:{port} succeeded"
            reasons = {4: "bad user name or password", 5: "not authorised"}
            return False, f"the relay refused the login ({reasons.get(connack[3], 'code %d' % connack[3])})"
    except ConnectionResetError:
        return False, "connection reset right after connecting (a VPN or firewall may be interfering; try the SSH tunnel option)"
    except socket.timeout:
        return False, f"timed out talking to {host}:{port}"
    except OSError as exc:
        return False, f"cannot reach {host}:{port} ({exc.strerror or exc})"


def send_usage(cfg, session, cost, context_pct=None):
    """Spend update from the status line, over the same transport as the events."""
    pct = "" if context_pct is None else f"{context_pct:g}"
    if cfg.get("hook_transport", "http") == "mqtt":
        send_mqtt(cfg.get("relay", {}), "usage", f"{session}:{cost:g}:{pct}".rstrip(":"))
    else:
        path = f"/usage/{session}/{cost:g}" + (f"/{pct}" if pct else "")
        req = urllib.request.Request(f"http://127.0.0.1:{int(cfg.get('http_port', 8787))}{path}", data=b"", method="POST")
        urllib.request.urlopen(req, timeout=1.5).close()


def main():
    if len(sys.argv) != 2:
        return
    event = sys.argv[1]
    session = read_session_id()
    cfg = load_config()
    transport = cfg.get("hook_transport", "http")
    try:
        if transport == "mqtt":
            send_mqtt(cfg.get("relay", {}), event, session)
        else:
            send_http(int(cfg.get("http_port", 8787)), event, session)
    except Exception:  # noqa: BLE001 - never disturb Claude Code
        pass


if __name__ == "__main__":
    main()
