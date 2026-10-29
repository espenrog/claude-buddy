#!/usr/bin/env python3
"""Claude Buddy bridge: receives Claude Code events and forwards them to the buddy over BLE.

Inputs (any combination, chosen in the config file):
  http  POST http://127.0.0.1:<port>/e/<event>[/<session>]   local mode, no broker needed
  mqtt  subscribe to a topic on an MQTT broker                relay mode (Claude on another machine, clones)

Output:
  BLE   two bytes per event (code, session slot), written to the buddy's event characteristic

Also serves a local web page (http://127.0.0.1:<port>/) with the buddy's status and settings, and can keep an
SSH tunnel to the broker alive (for networks that mangle plain MQTT). See docs/ARCHITECTURE.md and docs/PROTOCOL.md.
"""

import argparse
import asyncio
import collections
import io
import json
import logging
import logging.handlers
import os
import re
import subprocess
import sys
import time
import zipfile

BRIDGE_VERSION = "0.8.11"

SVC_UUID = "6b1d0001-5c3a-4e5b-9b2a-6c1a0d0e0001"
EVT_UUID = "6b1d0002-5c3a-4e5b-9b2a-6c1a0d0e0001"
VER_UUID = "6b1d0003-5c3a-4e5b-9b2a-6c1a0d0e0001"
CFG_UUID = "6b1d0004-5c3a-4e5b-9b2a-6c1a0d0e0001"
STATUS_UUID = "6b1d0005-5c3a-4e5b-9b2a-6c1a0d0e0001"
USAGE_UUID = "6b1d0006-5c3a-4e5b-9b2a-6c1a0d0e0001"  # firmware 0.4 and newer
TIME_UUID = "6b1d0007-5c3a-4e5b-9b2a-6c1a0d0e0001"  # firmware 0.6 and newer: local time for quiet hours
TIME_REFRESH_S = 600
BATTERY_UUID = "6b1d0008-5c3a-4e5b-9b2a-6c1a0d0e0001"  # firmware 0.7 and newer
LOG_UUID = "6b1d000a-5c3a-4e5b-9b2a-6c1a0d0e0001"  # firmware 0.8.7 and newer: the hourly battery log
LOG_REFRESH_S = 1800
MEASURE_UUID = "6b1d000b-5c3a-4e5b-9b2a-6c1a0d0e0001"  # firmware 0.8.8 and newer: hold one state for a power measurement

# Version of the bridge <-> buddy protocol this bridge speaks (2 = events carry a session slot),
# and the oldest firmware protocol it can still talk to. See docs/PROTOCOL.md.
PROTOCOL_VERSION = 2
MIN_PROTOCOL_VERSION = 1
MAX_SESSIONS = 8  # slots in the firmware; keep equal to MOOD_MAX_SESSIONS
SESSION_ID = re.compile(r"^[A-Za-z0-9_-]{1,64}$")

# One byte per event. Must match enum claude_event in firmware/src/mood.h.
EVENT_CODES = {
    "session_started": 0x01,
    "session_ended": 0x02,
    "prompt_submitted": 0x03,
    "turn_finished": 0x04,
    "needs_input": 0x05,
    "answered": 0x06,
    "error": 0x07,
    "limit_reached": 0x08,
    "idle": 0x09,
}

# Index = value in the buddy's status characteristic. Must match enum mood in firmware/src/mood.h.
MOOD_NAMES = ["sleeping", "chill", "bored", "working", "excited", "alert", "confused", "angry", "limit", "dizzy", "scared"]
FACE_PROFILES = 3

log = logging.getLogger("claude-buddy")


def time_wire(now=None):
    """[format, hour, minute] of the local time, for the buddy's quiet hours."""
    t = time.localtime(now)
    return bytes([1, t.tm_hour, t.tm_min])


def default_config_path():
    return os.path.join(os.path.expanduser("~"), ".config", "claude-buddy", "config.json")


def load_config(path):
    if not os.path.exists(path):
        log.warning("No config at %s, using defaults (HTTP input on 127.0.0.1:8787, BLE on)", path)
        return {}
    with open(path, encoding="utf-8") as f:
        return json.load(f)


# ---------------------------------------------------------------- settings (wire format: docs/PROTOCOL.md)

SETTINGS_DEFAULTS = {"brightness": 128, "sleep_brightness": 8, "face_profile": 0, "volume": 60,
                     "quiet_start_h": 22, "quiet_end_h": 7, "muted": False}


def pack_settings(values):
    """Validates a settings dict and returns the 8-byte wire format. Raises ValueError with a message."""
    def number(key, low, high):
        v = values.get(key)
        if isinstance(v, bool) or not isinstance(v, int):
            raise ValueError(f"{key} must be a whole number")
        if not low <= v <= high:
            raise ValueError(f"{key} must be between {low} and {high}")
        return v

    muted = values.get("muted")
    if not isinstance(muted, bool):
        raise ValueError("muted must be true or false")
    return bytes([
        1,  # format
        number("brightness", 1, 255),
        number("sleep_brightness", 0, 255),
        number("face_profile", 0, FACE_PROFILES - 1),
        number("volume", 0, 100),
        number("quiet_start_h", 0, 23),
        number("quiet_end_h", 0, 23),
        1 if muted else 0,
    ])


def unpack_settings(data):
    if len(data) != 8 or data[0] != 1:
        raise ValueError("unknown settings format")
    return {"brightness": data[1], "sleep_brightness": data[2], "face_profile": data[3], "volume": data[4],
            "quiet_start_h": data[5], "quiet_end_h": data[6], "muted": bool(data[7] & 1)}


def pack_battery(values):
    """Validates a battery dict and returns the 5-byte wire format (docs/PROTOCOL.md). Raises ValueError."""
    for key in ("present", "charging", "full"):
        if not isinstance(values.get(key), bool):
            raise ValueError(f"{key} must be true or false")
    percent, mv = values.get("percent"), values.get("mv", 0)
    for key, v, high in (("percent", percent, 100), ("mv", mv, 5000)):
        if isinstance(v, bool) or not isinstance(v, int) or not 0 <= v <= high:
            raise ValueError(f"{key} must be a whole number between 0 and {high}")
    if values["charging"] and values["full"]:
        raise ValueError("a battery is either charging or full, not both")
    if not values["present"] and (values["charging"] or values["full"] or percent or mv):
        raise ValueError("without a battery there is nothing else to report")
    flags = (1 if values["present"] else 0) | (2 if values["charging"] else 0) | (4 if values["full"] else 0)
    return bytes([1, flags, percent]) + mv.to_bytes(2, "little")


def unpack_battery(data):
    """The battery characteristic: five bytes (older firmware), eight (firmware 0.8.5: plus the battery current in mA,
    positive = charging, and the power chip's die temperature in degrees C) or nine (firmware 0.8.6: plus the cell's
    temperature in degrees C). What a firmware cannot measure is None."""
    if len(data) not in (5, 8, 9) or data[0] != 1 or data[1] & ~7:
        raise ValueError("unknown battery format")
    battery = {"present": bool(data[1] & 1), "charging": bool(data[1] & 2), "full": bool(data[1] & 4),
               "percent": data[2], "mv": int.from_bytes(data[3:5], "little"),
               "current_ma": None, "die_temp_c": None, "cell_temp_c": None}
    if len(data) >= 8:
        current = int.from_bytes(data[5:7], "little", signed=True)
        temp = int.from_bytes(data[7:8], "little", signed=True)
        battery["current_ma"] = None if current == -32768 else current
        battery["die_temp_c"] = None if temp == -128 else temp
    if len(data) == 9:
        cell = int.from_bytes(data[8:9], "little", signed=True)
        battery["cell_temp_c"] = None if cell == -128 else cell
    return battery


# ---------------------------------------------------------------- measurement modes (docs/PROTOCOL.md)

# Index = the mode byte. Must match enum measure_mode in firmware/src/measure_mode.h.
MEASURE_MODES = ["normal", "display_off", "face_still", "face_animated", "stats", "battery", "all_lit", "beep"]


def pack_measure(mode):
    """The write for a mode name: [format 1, mode]. Raises ValueError."""
    if mode not in MEASURE_MODES:
        raise ValueError("unknown measurement mode")
    return bytes([1, MEASURE_MODES.index(mode)])


def unpack_measure(data):
    """The read: [format 1, mode, minutes left]. Returns (mode name, minutes left). Raises ValueError."""
    if len(data) < 3 or data[0] != 1 or data[1] >= len(MEASURE_MODES):
        raise ValueError("unexpected measurement mode answer")
    return MEASURE_MODES[data[1]], data[2]


# ---------------------------------------------------------------- hourly battery log (docs/PROTOCOL.md)

BATTERY_LOG_COLUMNS = ["epoch", "seq", "hour", "fetched_at", "percent", "mv", "avg_current_ma", "used_mah",
                       "display_on_min", "on_usb"]


def unpack_log_answer(data):
    """The log characteristic: newest seq (u32), records held (u16), then the selected record (13 bytes) if there is one.
    Returns (newest, count, record or None). Raises ValueError."""
    if len(data) not in (6, 19):
        raise ValueError("unexpected battery log length")
    newest, count = int.from_bytes(data[0:4], "little"), int.from_bytes(data[4:6], "little")
    if len(data) == 6:
        return newest, count, None
    seq = int.from_bytes(data[6:10], "little")
    hour, flags, percent = data[10], data[11], data[12]
    mv = int.from_bytes(data[13:15], "little")
    current = int.from_bytes(data[15:17], "little", signed=True) / 100.0  # units of 10 microamperes -> mA
    lit_s = int.from_bytes(data[17:19], "little")
    return newest, count, {"seq": seq, "hour": None if hour == 0xFF else hour, "on_usb": bool(flags & 1),
                           "percent": percent, "mv": mv, "avg_current_ma": current, "display_on_min": round(lit_s / 60, 1)}


class BatteryLogFile:
    """Appends the buddy's hourly records to a CSV file. Keeps what it has already written, so the same record is not
    fetched twice. When the buddy's numbering starts over (new flash, erased storage) a new epoch begins."""

    def __init__(self, path=None):
        self.configure(path)

    def configure(self, path):
        self.path, self.epoch, self.last_seq = path, 0, 0
        if path is None:
            return
        try:
            with open(path, encoding="utf-8") as f:
                for line in list(f)[1:]:
                    cols = line.rstrip("\n").split(",")
                    self.epoch, self.last_seq = int(cols[0]), int(cols[1])
        except (OSError, ValueError, IndexError):
            pass

    def want_from(self, newest, count):
        """The first seq to fetch, given what the buddy reports (it holds at most `count` of the newest)."""
        if newest < self.last_seq:  # numbering started over
            self.epoch, self.last_seq = self.epoch + 1, 0
        return max(self.last_seq + 1, newest - count + 1, 1)

    def append(self, rec, fetched_at):
        new_file = not os.path.exists(self.path)
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        current = rec["avg_current_ma"]
        used = "" if rec["on_usb"] or current >= 0 else f"{-current:.2f}"  # mAh in this hour, only when running on the cell
        row = [self.epoch, rec["seq"], "" if rec["hour"] is None else rec["hour"], fetched_at, rec["percent"], rec["mv"],
               f"{current:.2f}", used, rec["display_on_min"], int(rec["on_usb"])]
        with open(self.path, "a", encoding="utf-8") as f:
            if new_file:
                f.write(",".join(BATTERY_LOG_COLUMNS) + "\n")
            f.write(",".join(str(c) for c in row) + "\n")
        self.last_seq = rec["seq"]


async def fetch_battery_log(client, store):
    """Reads the records the buddy holds that are not in the file yet. Returns how many were added."""
    from_seq = None
    added = 0
    stamp = time.strftime("%Y-%m-%dT%H:%M:%S")
    newest, count, _ = unpack_log_answer(bytes(await client.read_gatt_char(LOG_UUID)))
    if newest == 0:
        return 0
    from_seq = store.want_from(newest, count)
    for seq in range(from_seq, newest + 1):
        await client.write_gatt_char(LOG_UUID, seq.to_bytes(4, "little"), response=True)
        _, _, rec = unpack_log_answer(bytes(await client.read_gatt_char(LOG_UUID)))
        if rec is None or rec["seq"] != seq:  # overwritten or missing: skip it
            continue
        store.append(rec, stamp)
        added += 1
    return added


def firmware_version(data):
    """The firmware version text from the version characteristic: "major.minor.patch" when the
    firmware sends the patch byte (0.8.4 and later), otherwise "major.minor"."""
    text = f"{data[1]}.{data[2]}"
    return f"{text}.{data[4]}" if len(data) >= 5 else text


def describe_firmware(data):
    """Interprets the buddy's version characteristic: [protocol, fw major, fw minor, capabilities, fw patch].
    Returns (ok, message). `data` is None when the buddy has no such characteristic."""
    if data is None or len(data) < 3:
        return False, ("the buddy's firmware is older than 0.2 and reports no version; "
                       "it still works, but update it when convenient")
    protocol, major, minor = data[0], data[1], data[2]
    fw = f"firmware {firmware_version(data)}"
    if protocol == PROTOCOL_VERSION:
        return True, f"{fw}, protocol {protocol}"
    if protocol > PROTOCOL_VERSION:
        return False, f"{fw} speaks protocol {protocol}, newer than this bridge ({PROTOCOL_VERSION}): re-run install.py to update the bridge"
    if protocol >= MIN_PROTOCOL_VERSION:
        return True, f"{fw}, protocol {protocol} (works, but several-session display needs firmware 0.3)"
    return False, f"{fw} speaks protocol {protocol}, older than this bridge supports: update the buddy's firmware"


class SessionSlots:
    """Maps Claude session ids to the firmware's session slots (least recently used is recycled)."""

    def __init__(self, size=MAX_SESSIONS):
        self.size = size
        self.order = []  # session ids, least recently used first
        self.slots = {}  # session id -> slot

    def slot_for(self, session_id):
        """Returns (slot, recycled): `recycled` is True if the slot belonged to another session."""
        recycled = False
        if session_id in self.slots:
            self.order.remove(session_id)
        elif len(self.slots) < self.size:
            self.slots[session_id] = next(i for i in range(self.size) if i not in self.slots.values())
        else:
            oldest = self.order.pop(0)
            self.slots[session_id] = self.slots.pop(oldest)
            recycled = True
        self.order.append(session_id)
        return self.slots[session_id], recycled

    def end(self, session_id):
        self.slots.pop(session_id, None)
        if session_id in self.order:
            self.order.remove(session_id)


def parse_event(text):
    """'name' or 'name:session_id' -> (name, session_id or None)."""
    name, _, session = text.strip().partition(":")
    return name, (session if SESSION_ID.match(session) else None)


class BridgeState:
    """What the web page shows. Written by the BLE loop and the input handlers, read by the web server."""

    def __init__(self):
        self.started = time.time()
        self.events = collections.deque(maxlen=30)
        self.pairing_hint = None  # set while the buddy wants pairing and we are not paired yet; kept across reconnects
        self.reset_link()

    def reset_link(self):
        self.ble = False
        self.buddy = None  # the name (or address) of the buddy we are connected to
        self.simulated = False
        self.firmware = None
        self.protocol = None
        self.settings = None
        self.status = None
        self.battery = None
        self.can_update = False  # the application takes firmware updates itself (SMP service)
        self.address = None  # the Bluetooth address of the connected buddy (the in-app update connects to it again)
        self.can_simulate_battery = False
        self.can_measure = False
        self.measure_mode = None
        self.measure_until = None  # time.monotonic() when the buddy ends the held mode by itself

    def set_measure(self, mode, minutes):
        self.measure_mode = None if mode == "normal" else mode
        self.measure_until = None if mode == "normal" else time.monotonic() + minutes * 60

    def measure_snapshot(self):
        st = self.status
        if not self.can_measure or not st or not st.get("measuring"):
            return None  # the buddy ended the mode itself (or no mode was ever set)
        left = None if self.measure_until is None else max(0, int((self.measure_until - time.monotonic() + 59) // 60))
        return {"mode": self.measure_mode, "minutes_left": left}

    def set_status(self, data):
        if len(data) >= 5:
            self.status = {"mood": data[0], "active": data[1], "working": data[2], "waiting": data[3],
                           "display_off": bool(data[4] & 1), "muted": bool(data[4] & 2),
                           "measuring": bool(data[4] & 4)}

    def set_battery(self, data):
        try:
            self.battery = unpack_battery(data)
        except ValueError:
            pass

    def snapshot(self):
        st = self.status
        mood = MOOD_NAMES[st["mood"]] if st and st["mood"] < len(MOOD_NAMES) else None
        return {
            "bridge_version": BRIDGE_VERSION,
            "ble": self.ble,
            "buddy": self.buddy,
            "pairing_hint": self.pairing_hint,
            "simulated": self.simulated,
            "firmware": self.firmware,
            "protocol": self.protocol,
            "mood": mood,
            "sessions": {k: st[k] for k in ("active", "working", "waiting")} if st else None,
            "display_off": st["display_off"] if st else None,
            "settings": self.settings,
            "usage": USAGE.snapshot(),
            "battery": self.battery,
            "can_update": self.can_update,
            "update": UPDATE.snapshot(),
            "can_simulate_battery": self.can_simulate_battery,
            "can_measure": self.can_measure,
            "measure": self.measure_snapshot(),
            "events": list(self.events),
            "uptime_s": int(time.time() - self.started),
        }


STATE = BridgeState()
SESSIONS = SessionSlots()


def submit(text, queue, slots=None):
    """Validates an event ('name' or 'name:session_id') and queues (code, slot). True if accepted."""
    name, session = parse_event(text)
    code = EVENT_CODES.get(name)
    if code is None:
        log.warning("Ignoring unknown event %r", name)
        return False
    slots = slots if slots is not None else SESSIONS
    slot, recycled = slots.slot_for(session or "manual")
    if recycled and code != EVENT_CODES["session_started"]:
        queue.put_nowait((EVENT_CODES["session_started"], slot))  # clear the previous owner's state
    log.info("event %s -> 0x%02x (slot %d)", name, code, slot)
    STATE.events.appendleft({"t": time.strftime("%H:%M:%S"), "event": name, "slot": slot})
    queue.put_nowait((code, slot))
    if name == "session_ended":
        slots.end(session or "manual")
    return True


# ---------------------------------------------------------------- usage and expenses (fed by the status line)


class UsageStore:
    """Estimated spend, kept in a small state file. The month total is the sum of each session's largest cost
    seen, so missed updates, restarts and several bridges all agree. Calibration ties that sum to the figure the Claude app
    shows: the estimate at that moment becomes the app's number, and spend after it is scaled by the ratio between how
    much the app's figure and the estimate grew since the previous calibration (1.0 until there are two)."""

    SAVE_EVERY = 10  # seconds between disk writes
    MIN_SPAN = 5.0  # usd of estimated spend between two calibrations before the ratio is trusted

    def __init__(self, path=None, now=time.time):
        self.path, self.now = path, now
        self.sessions = {}  # id -> {"max": usd, "month": "YYYY-MM", "day": "YYYY-MM-DD", "day_start": usd, "ctx": percent}
        self.budget = {"monthly_usd": 0.0, "warn_pct": 80}
        self.calib = {}  # "YYYY-MM" -> {"raw": estimate when calibrated, "real": the app's figure, "factor": scale for later spend}
        self.dirty, self.saved_at = False, 0.0
        self.load()

    def _stamp(self):
        t = time.localtime(self.now())
        return time.strftime("%Y-%m", t), time.strftime("%Y-%m-%d", t)

    def load(self):
        if not self.path:
            return
        try:
            with open(self.path, encoding="utf-8") as f:
                data = json.load(f)
            self.sessions = {k: v for k, v in data.get("sessions", {}).items() if isinstance(v, dict)}
            self.budget.update(data.get("budget", {}))
            self.calib = {m: c for m, c in data.get("calib", {}).items() if isinstance(c, dict)}
            for m, off in data.get("offsets", {}).items():  # older state files: a plain offset is a calibration at raw 0
                self.calib.setdefault(m, {"raw": 0.0, "real": float(off), "factor": 1.0})
        except (OSError, ValueError):
            pass

    def save(self, force=False):
        if not self.path or not (self.dirty or force):
            return
        if not force and self.now() - self.saved_at < self.SAVE_EVERY:
            return
        month, _ = self._stamp()
        keep = {sid: v for sid, v in self.sessions.items() if v.get("month", month) >= self._months_ago(2)}
        self.sessions = keep
        tmp = self.path + ".tmp"
        try:
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump({"sessions": keep, "budget": self.budget, "calib": self.calib}, f)
            os.replace(tmp, self.path)
            self.dirty, self.saved_at = False, self.now()
        except OSError as exc:
            log.warning("Cannot save usage to %s: %s", self.path, exc)

    def _months_ago(self, n):
        t = time.localtime(self.now())
        y, m = t.tm_year, t.tm_mon - n
        while m < 1:
            y, m = y - 1, m + 12
        return f"{y:04d}-{m:02d}"

    def record(self, session, cost, ctx_pct=None):
        """Returns True if the numbers were accepted."""
        if not SESSION_ID.match(session or "") or isinstance(cost, bool) or not isinstance(cost, (int, float)):
            return False
        if not 0 <= cost <= 100000:
            return False
        month, day = self._stamp()
        rec = self.sessions.setdefault(session, {"max": 0.0, "month": month, "day": day, "day_start": 0.0})
        if rec.get("day") != day:  # first update today: what was spent before counts for earlier days
            rec["day"], rec["day_start"] = day, rec["max"]
        if cost > rec["max"]:
            rec["max"] = round(float(cost), 4)
        if isinstance(ctx_pct, (int, float)) and not isinstance(ctx_pct, bool) and 0 <= ctx_pct <= 100:
            rec["ctx"] = round(float(ctx_pct), 1)
        self.dirty = True
        self.save()
        return True

    def month_usd(self):
        month, _ = self._stamp()
        raw = sum(v["max"] for v in self.sessions.values() if v.get("month") == month)
        c = self.calib.get(month)
        if not c:
            return raw, raw
        return raw, c["real"] + max(0.0, raw - c["raw"]) * c["factor"]

    def _last_factor(self, month):
        """The scale from this month's calibration, else from the latest earlier month's, else 1.0."""
        for m in sorted((m for m in self.calib if m <= month), reverse=True):
            return self.calib[m]["factor"]
        return 1.0

    def today_usd(self):
        _, day = self._stamp()
        return sum(v["max"] - v.get("day_start", 0.0) for v in self.sessions.values() if v.get("day") == day)

    def calibrate(self, month_usd):
        if isinstance(month_usd, bool) or not isinstance(month_usd, (int, float)) or not 0 <= month_usd <= 1000000:
            raise ValueError("month_usd must be a number between 0 and 1000000")
        month, _ = self._stamp()
        raw, _ = self.month_usd()
        prev = self.calib.get(month)
        factor = self._last_factor(month)
        if prev and raw - prev["raw"] >= self.MIN_SPAN and month_usd > prev["real"]:  # enough spend to measure the ratio
            factor = min(5.0, max(0.5, (month_usd - prev["real"]) / (raw - prev["raw"])))
        self.calib[month] = {"raw": round(raw, 4), "real": round(float(month_usd), 4), "factor": round(factor, 4)}
        self.save(force=True)

    def set_budget(self, monthly_usd, warn_pct):
        if isinstance(monthly_usd, bool) or not isinstance(monthly_usd, (int, float)) or not 0 <= monthly_usd <= 1000000:
            raise ValueError("monthly_usd must be a number between 0 and 1000000 (0 turns the budget off)")
        if isinstance(warn_pct, bool) or not isinstance(warn_pct, int) or not 1 <= warn_pct <= 100:
            raise ValueError("warn_pct must be a whole number between 1 and 100")
        self.budget = {"monthly_usd": float(monthly_usd), "warn_pct": warn_pct}
        self.save(force=True)

    def wire(self, amounts=False):
        """The usage characteristic for the buddy (see docs/PROTOCOL.md). Format 1, firmware 0.4: [1, flags, percent of
        budget]. Format 2, firmware 0.5 and newer, adds this month's and today's cents for the stats page."""
        snap = self.snapshot()
        flags, pct = 0, 0
        if snap and snap["budget_usd"]:
            flags = 0x01 | (0x02 if snap["over_warning"] else 0)
            pct = min(255, snap["budget_used_pct"])
        if not amounts:
            return bytes([1, flags, pct])
        month = min(2**32 - 1, round(snap["month_usd"] * 100)) if snap else 0
        today = min(65535, max(0, round(snap["today_usd"] * 100))) if snap else 0
        return bytes([2, flags, pct]) + month.to_bytes(4, "little") + today.to_bytes(2, "little")

    def snapshot(self):
        if not self.sessions and not self.calib and not self.budget["monthly_usd"]:
            return None
        _, month = self.month_usd()
        limit = self.budget["monthly_usd"]
        pct = round(100 * month / limit) if limit else None
        ctx = [v["ctx"] for v in self.sessions.values() if v.get("day") == self._stamp()[1] and "ctx" in v]
        return {"month_usd": round(month, 2), "today_usd": round(self.today_usd(), 2),
                "budget_usd": limit, "warn_pct": self.budget["warn_pct"], "budget_used_pct": pct,
                "over_warning": bool(limit and pct >= self.budget["warn_pct"]),
                "context_pct": max(ctx) if ctx else None}


USAGE = UsageStore()
BATTERY_LOG = BatteryLogFile()


def ingest(text, queue):
    """One text message from a hook or the relay: an event name, or 'usage:<session>:<usd>:<context %>'."""
    if text.startswith("usage:"):
        parts = text.split(":")
        try:
            ok = len(parts) in (3, 4) and USAGE.record(parts[1], float(parts[2]), float(parts[3]) if len(parts) == 4 and parts[3] else None)
        except ValueError:
            ok = False
        return ok
    return submit(text, queue)


# ---------------------------------------------------------------- web server and hook endpoint

# ---------------------------------------------------------------- firmware update over Bluetooth

APP_SLOT_BYTES = 0xf2000  # the application slot on the XIAO nRF54LM20A (968 kB)
MAX_UPLOAD = 2 * 1024 * 1024
MCUBOOT_MAGIC = bytes.fromhex("3db8f396")  # 0x96f3b83d, little endian: first word of a signed MCUboot image


class UpdateError(Exception):
    """A firmware update step failed; the message is shown on the web page."""


def extract_app_image(data):
    """Returns the signed application image from a DFU package (the zip the build makes) or a bare signed .bin.
    Raises ValueError with a message for the web page. The signature itself is checked by MCUboot on the buddy."""
    if not data:
        raise ValueError("the file is empty or larger than 2 MB")
    image = data
    if data[:4] == b"PK\x03\x04":
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as zf:
                manifest = json.loads(zf.read("manifest.json"))
                files = manifest.get("files") if isinstance(manifest, dict) else None
                if not isinstance(files, list) or not files:
                    raise ValueError("the package's manifest.json lists no images")
                if len(files) != 1 or not isinstance(files[0], dict) or str(files[0].get("image_index", "0")) != "0":
                    raise ValueError("the package holds more than the application image: only the application can be sent over Bluetooth")
                name = files[0].get("file")
                if not isinstance(name, str) or zf.getinfo(name).file_size > APP_SLOT_BYTES:
                    raise ValueError("the package's application image is missing or too large")
                image = zf.read(name)
        except (zipfile.BadZipFile, KeyError, UnicodeDecodeError) as exc:
            raise ValueError("not a firmware package (needs manifest.json and the image file)") from exc
        except json.JSONDecodeError as exc:
            raise ValueError("not a firmware package (manifest.json is not valid)") from exc
    if image[:4] != MCUBOOT_MAGIC:
        raise ValueError("not a signed application image (use the dfu_application.zip or the .signed.bin from the build)")
    if len(image) > APP_SLOT_BYTES:
        raise ValueError(f"the image is {len(image) // 1024} kB, more than the {APP_SLOT_BYTES // 1024} kB application slot")
    return image


class UpdateJob:
    """Progress of the firmware update, shown on the web page. While `busy` the BLE loop leaves the radio alone."""

    SHOW_DONE_S, SHOW_ERROR_S = 20, 120  # how long the web page keeps reporting the outcome

    def __init__(self):
        self.state, self.percent, self.message, self.task, self.since = "idle", 0, "", None, 0.0

    @property
    def busy(self):
        return self.state not in ("idle", "done", "error")

    def set(self, state, message="", percent=None):
        self.state, self.message, self.since = state, message, time.monotonic()
        if percent is not None:
            self.percent = percent

    def snapshot(self):
        shown_for = {"done": self.SHOW_DONE_S, "error": self.SHOW_ERROR_S}.get(self.state)
        if shown_for is not None and time.monotonic() - self.since > shown_for:
            self.state, self.message = "idle", ""  # the outcome has been on show long enough
        return {"state": self.state, "busy": self.busy, "percent": self.percent, "message": self.message}


UPDATE = UpdateJob()


def pending_image_hash(images):
    """The hash of the image just uploaded into the second slot, from an image state response; None if there is none."""
    for image in images or []:
        if getattr(image, "slot", None) == 1 and getattr(image, "hash", None):
            return bytes(image.hash)
    return None


async def smp_upload(image, progress, address):
    """Uploads the image over SMP to the buddy at `address` and restarts it into the new application. `progress` is
    called with the number of bytes sent. The image goes to the second slot, is marked for a test run, and MCUboot swaps
    it in at the restart; the new firmware keeps itself once its Bluetooth works, otherwise the next restart goes back
    to the old one."""
    try:
        from smpclient import SMPClient
        from smpclient.generics import error, success
        from smpclient.requests.image_management import ImageStatesRead, ImageStatesWrite
        from smpclient.requests.os_management import ResetWrite
        from smpclient.transport.ble import SMPBLETransport
    except ImportError as exc:
        raise UpdateError("the update library (smpclient, needs Python 3.10 or newer) is not installed: "
                          "re-run install.py, or install it with pip") from exc
    UPDATE.set("searching", "Connecting to the buddy for the update…")
    client = SMPClient(SMPBLETransport(), address, timeout_s=10)
    try:
        await client.connect(connect_timeout_s=30)
        UPDATE.set("uploading", "Sending the firmware…", 0)
        async for offset in client.upload(image):
            progress(offset)
        UPDATE.set("resetting", "Marking the new firmware for a test run…", 100)
        states = await client.request(ImageStatesRead(), timeout_s=10)
        if error(states):
            raise UpdateError(f"the buddy did not list its images: {states}")
        digest = pending_image_hash(states.images)
        if digest is None:
            raise UpdateError("the buddy holds no new image after the upload (was the file signed with its key?)")
        marked = await client.request(ImageStatesWrite(hash=digest, confirm=False), timeout_s=10)
        if not success(marked):
            raise UpdateError(f"the buddy refused the new image: {marked}")
        UPDATE.set("resetting", "Restarting the buddy…", 100)
        try:
            await client.request(ResetWrite(), timeout_s=5)
        except Exception as exc:  # noqa: BLE001 - the buddy drops the link while it restarts
            log.debug("reset request ended with %r (expected)", exc)
    except UpdateError:
        raise
    except Exception as exc:  # noqa: BLE001
        raise UpdateError(f"the update failed: {exc}") from exc
    finally:
        try:
            await client.disconnect()
        except Exception:  # noqa: BLE001
            pass


async def fake_upload(image, progress, address):
    """What the simulated buddy does instead of a real upload, so the web page can be tried without hardware."""
    UPDATE.set("uploading", "Sending the firmware (simulated)…", 0)
    for step in range(1, 11):
        await asyncio.sleep(0.05)
        progress(len(image) * step // 10)
    UPDATE.set("resetting", "Restarting the buddy (simulated)…", 100)


async def run_update(image, upload=None):
    """Lets go of the connected buddy, uploads `image` to it over a new connection, and records the outcome in UPDATE."""
    upload = upload or (fake_upload if STATE.simulated else smp_upload)
    address = STATE.address

    def progress(offset):
        UPDATE.percent = min(100, offset * 100 // len(image))

    async def job():
        # The BLE loop lets go of the link when it sees "releasing" (and stays off while UPDATE is busy), then the
        # update connects to the same buddy. The paired link is encrypted again from the stored keys.
        UPDATE.set("releasing", "Letting go of the buddy for the update…", 0)
        if not STATE.simulated:
            for _ in range(80):
                if not STATE.ble:
                    break
                await asyncio.sleep(0.25)
            else:
                raise UpdateError("the bridge could not release the buddy's link for the update")
        await upload(image, progress, address)

    try:
        await asyncio.wait_for(job(), 600)
        UPDATE.set("done", "Update sent. The buddy restarts with the new firmware and reconnects in a moment"
                   " (if the new firmware fails to start, the next restart goes back to the old one).", 100)
        log.info("firmware update sent (%d bytes)", len(image))
    except UpdateError as exc:
        UPDATE.set("error", str(exc))
        log.error("firmware update failed: %s", exc)
    except asyncio.TimeoutError:
        UPDATE.set("error", "the update took too long and was stopped")
        log.error("firmware update timed out")
    except Exception as exc:  # noqa: BLE001
        UPDATE.set("error", f"the update failed: {exc!r}")
        log.error("firmware update failed: %r", exc)


HTTP_REASONS = {200: "OK", 202: "Accepted", 204: "No Content", 400: "Bad Request", 403: "Forbidden",
                404: "Not Found", 405: "Method Not Allowed", 409: "Conflict", 503: "Service Unavailable"}
MAX_BODY = 4096
WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
PAGE_HEADERS = {
    "Content-Security-Policy": "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'",
    "X-Content-Type-Options": "nosniff",
    "X-Frame-Options": "DENY",
    "Referrer-Policy": "no-referrer",
    "Cache-Control": "no-store",
}


async def read_request(reader):
    line = await asyncio.wait_for(reader.readline(), 5)
    parts = line.decode("latin-1").split()
    headers = {}
    for _ in range(64):
        raw = await asyncio.wait_for(reader.readline(), 5)
        if raw in (b"\r\n", b"\n", b""):
            break
        key, _, value = raw.decode("latin-1").partition(":")
        headers[key.strip().lower()] = value.strip()
    try:
        length = int(headers.get("content-length", "0") or 0)
    except ValueError:
        length = 0
    body = b""
    upload = len(parts) >= 2 and parts[1].split("?")[0] == "/api/update"  # the only request with a large body
    if 0 < length <= (MAX_UPLOAD if upload else MAX_BODY):
        body = await asyncio.wait_for(reader.readexactly(length), 30 if upload else 5)
    return parts, headers, body


def respond(status, body=b"", content_type="application/json", extra=None):
    if isinstance(body, (dict, list)):
        body = json.dumps(body).encode()
    head = [f"HTTP/1.1 {status} {HTTP_REASONS.get(status, '')}", f"Content-Length: {len(body)}",
            "Connection: close"]
    if body:
        head.append(f"Content-Type: {content_type}")
    for k, v in (extra or {}).items():
        head.append(f"{k}: {v}")
    return ("\r\n".join(head) + "\r\n\r\n").encode() + body


def request_allowed(headers, port):
    """Only requests addressed to this machine, from pages we served (or from non-browser tools)."""
    hosts = {f"127.0.0.1:{port}", f"localhost:{port}", "127.0.0.1", "localhost"}
    if headers.get("host", "") not in hosts:
        return False  # DNS rebinding
    origin = headers.get("origin")
    return origin is None or origin in {f"http://127.0.0.1:{port}", f"http://localhost:{port}"}


class WebContext:
    def __init__(self, port, queue, settings_queue):
        self.port = port
        self.queue = queue
        self.settings_queue = settings_queue


def route(method, path, headers, body, ctx):
    """Returns the full HTTP response for one request."""
    if not request_allowed(headers, ctx.port):
        return respond(403, {"error": "forbidden"})

    # Hook endpoint, used by emit.py and curl
    if path.startswith("/e/"):
        if method != "POST":
            return respond(404, {"error": "not found"})
        name, _, session = path[3:].partition("/")
        ok = submit(f"{name}:{session}" if session else name, ctx.queue)
        return respond(204 if ok else 400)

    if path.startswith("/usage/"):  # status-line forwarder: /usage/<session>/<usd>[/<context %>]
        parts = path.split("/")[2:]
        try:
            ok = len(parts) in (2, 3) and USAGE.record(parts[0], float(parts[1]), float(parts[2]) if len(parts) == 3 else None)
        except ValueError:
            ok = False
        return respond(204 if ok and method == "POST" else 400)

    if method == "GET" and path in ("/", "/index.html"):
        try:
            with open(os.path.join(WEB_DIR, "index.html"), "rb") as f:
                return respond(200, f.read(), "text/html; charset=utf-8", PAGE_HEADERS)
        except OSError:
            return respond(404, b"web page not installed (re-run install.py)", "text/plain")
    if method == "GET" and path == "/api/status":
        return respond(200, STATE.snapshot(), extra={"Cache-Control": "no-store"})

    if path.startswith("/api/"):
        if method != "POST":
            return respond(405, {"error": "method not allowed"})
        if headers.get("x-claude-buddy") != "1":
            return respond(403, {"error": "missing X-Claude-Buddy header"})
        if path == "/api/settings":
            try:
                data = pack_settings(json.loads(body.decode("utf-8")))
            except (ValueError, UnicodeDecodeError, AttributeError) as exc:
                return respond(400, {"error": str(exc)})
            if not STATE.ble:
                return respond(503, {"error": "the buddy is not connected"})
            ctx.settings_queue.put_nowait(("settings", data))
            return respond(202, {"ok": True})
        if path == "/api/update":  # a firmware package, uploaded to the buddy by the bridge
            if UPDATE.busy:
                return respond(409, {"error": "an update is already running"})
            try:
                image = extract_app_image(body)
            except ValueError as exc:
                return respond(400, {"error": str(exc)})
            if not STATE.ble:
                return respond(503, {"error": "the buddy is not connected"})
            if not STATE.can_update:
                return respond(409, {"error": "this buddy's firmware cannot be updated over Bluetooth"})
            UPDATE.set("preparing", "Starting…", 0)
            UPDATE.task = asyncio.get_running_loop().create_task(run_update(image))
            return respond(202, {"ok": True, "bytes": len(image)})
        if path == "/api/measure":  # hold one state for a power measurement
            try:
                data = pack_measure(json.loads(body.decode("utf-8")).get("mode"))
            except (ValueError, UnicodeDecodeError, AttributeError) as exc:
                return respond(400, {"error": str(exc)})
            if not STATE.ble:
                return respond(503, {"error": "the buddy is not connected"})
            if not STATE.can_measure:
                return respond(409, {"error": "this buddy's firmware has no measurement modes (needs 0.8.8)"})
            ctx.settings_queue.put_nowait(("measure", data))
            return respond(202, {"ok": True})
        if path == "/api/battery":  # test input for a buddy without a fuel gauge
            try:
                data = pack_battery(json.loads(body.decode("utf-8")))
            except (ValueError, UnicodeDecodeError, AttributeError) as exc:
                return respond(400, {"error": str(exc)})
            if not STATE.ble:
                return respond(503, {"error": "the buddy is not connected"})
            if not STATE.can_simulate_battery:
                return respond(409, {"error": "this buddy has a real battery gauge and ignores simulated readings"})
            ctx.settings_queue.put_nowait(("battery", data))
            return respond(202, {"ok": True})
        if path in ("/api/budget", "/api/calibrate"):
            try:
                data = json.loads(body.decode("utf-8"))
                if path == "/api/budget":
                    USAGE.set_budget(data.get("monthly_usd"), data.get("warn_pct", 80))
                else:
                    USAGE.calibrate(data.get("month_usd"))
            except (ValueError, UnicodeDecodeError, AttributeError) as exc:
                return respond(400, {"error": str(exc)})
            return respond(200, {"ok": True, "usage": USAGE.snapshot()})
        if path.startswith("/api/test/"):
            ok = submit(path[len("/api/test/"):], ctx.queue)
            return respond(202 if ok else 400, {"ok": ok})
    return respond(404, {"error": "not found"})


async def handle_http(reader, writer, ctx):
    try:
        parts, headers, body = await read_request(reader)
        if len(parts) >= 2:
            writer.write(route(parts[0], parts[1].split("?")[0], headers, body, ctx))
        else:
            writer.write(respond(400, {"error": "bad request"}))
        await writer.drain()
    except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError, OSError):
        pass
    finally:
        writer.close()


async def start_http(port, queue, settings_queue):
    ctx = WebContext(int(port), queue, settings_queue)
    server = await asyncio.start_server(lambda r, w: handle_http(r, w, ctx), "127.0.0.1", int(port))
    log.info("HTTP and web page listening on http://127.0.0.1:%d/", int(port))
    return server


# ---------------------------------------------------------------- MQTT input, SSH tunnel


def start_mqtt(cfg, queue, loop, host_override=None, port_override=None):
    import paho.mqtt.client as mqtt

    if hasattr(mqtt, "CallbackAPIVersion"):
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    else:  # paho-mqtt 1.x
        client = mqtt.Client()
    topic = cfg.get("topic", "claude-buddy/events")
    if cfg.get("user"):
        client.username_pw_set(cfg["user"], cfg.get("password", ""))

    def on_connect(cl, userdata, flags, rc, props=None):
        if getattr(rc, "is_failure", rc != 0):
            log.error("MQTT connect refused: %s", rc)
            return
        log.info("MQTT connected, subscribing to %s", topic)
        cl.subscribe(topic)

    def on_disconnect(cl, userdata, *args):
        log.warning("MQTT disconnected (will retry)")

    def on_message(cl, userdata, msg):
        text = msg.payload.decode(errors="replace").strip()
        loop.call_soon_threadsafe(ingest, text, queue)

    client.on_connect, client.on_disconnect, client.on_message = on_connect, on_disconnect, on_message
    client.reconnect_delay_set(min_delay=1, max_delay=30)
    host = host_override or cfg.get("host", "localhost")
    port = port_override or int(cfg.get("port", 1883))
    log.info("MQTT input connecting to %s:%d", host, port)
    client.connect_async(host, port, keepalive=30)
    client.loop_start()
    return client


async def run_tunnel(cfg):
    """Keeps `ssh -L` to the broker host alive. Needs key-based (non-interactive) login."""
    local_port = int(cfg.get("local_port", 11883))
    remote_port = int(cfg.get("remote_port", 1883))
    cmd = [
        "ssh", "-N",
        "-L", f"127.0.0.1:{local_port}:127.0.0.1:{remote_port}",
        "-o", "ExitOnForwardFailure=yes",
        "-o", "ServerAliveInterval=30",
        "-o", "ServerAliveCountMax=3",
        "-o", "BatchMode=yes",
        "-o", "StrictHostKeyChecking=accept-new",
    ]
    if cfg.get("identity_file"):
        cmd += ["-i", cfg["identity_file"]]
    cmd.append(cfg["target"])
    flags = 0x08000000 if sys.platform == "win32" else 0  # CREATE_NO_WINDOW
    while True:
        log.info("Starting SSH tunnel: %s", " ".join(cmd))
        proc = await asyncio.create_subprocess_exec(
            *cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, creationflags=flags,
        )
        _, err = await proc.communicate()
        log.warning("SSH tunnel exited (%s): %s", proc.returncode, err.decode(errors="replace").strip())
        await asyncio.sleep(5)


# ---------------------------------------------------------------- BLE output


def pick_buddy(found, wanted=None):
    """Chooses one buddy from a scan. found: [(device, advertisement data)] that offer our service.

    wanted is the name, or the 4-digit id at the end of the name, or the address from the config; it picks that one
    (None if it is not in range). With no wanted and several buddies in range the nearest one (strongest signal) wins.
    Returns (device, others in range) or (None, []).
    """
    if not found:
        return None, []
    want = (wanted or "").strip().lower()

    def label(item):
        return item[1].local_name or item[0].name or item[0].address

    if want:
        matches = [i for i in found if want in (label(i).lower(), label(i).lower().rsplit(" ", 1)[-1], i[0].address.lower())]
    else:
        matches = sorted(found, key=lambda i: i[1].rssi if i[1].rssi is not None else -999, reverse=True)[:1]
    if not matches:
        return None, []
    return matches[0][0], [label(i) for i in found if i is not matches[0]]


async def find_buddy(wanted, scan_s=6):
    from bleak import BleakScanner

    seen = await BleakScanner.discover(timeout=scan_s, return_adv=True)
    found = [(d, a) for d, a in seen.values() if SVC_UUID in [u.lower() for u in (a.service_uuids or [])]]
    device, others = pick_buddy(found, wanted)
    if device is None and found:
        names = ", ".join(sorted(a.local_name or d.address for d, a in found))
        log.warning("No buddy named %r in range (found: %s)", wanted, names)
    elif others:
        log.warning("More than one buddy in range: using the %s one, ignoring %s. Set bridge.ble_name in the config "
                    "(e.g. \"A1B2\") to choose.", "named" if wanted else "nearest", ", ".join(others))
    return device


PAIRING_CAP = 0x10  # version characteristic, capability byte: the buddy needs a paired (encrypted) link
SMP_UPDATE_CAP = 0x20  # the application takes firmware updates itself (SMP service)
PAIRING_HINT = "Not paired yet: hold the buddy's mute button for 3 seconds, then wait a moment"


class PairingNeeded(Exception):
    pass


async def secure_link(client, version):
    """Makes sure the link is paired and encrypted when the buddy needs that. Raises PairingNeeded if it cannot be.

    The version characteristic is open; everything else is not. We probe with the settings characteristic, and on a
    refusal ask the operating system to pair (the buddy only agrees while its pairing window is open).
    """
    if not (version and len(version) >= 4 and version[3] & PAIRING_CAP):
        return
    for attempt in range(2):
        try:
            await client.read_gatt_char(CFG_UUID)
            STATE.pairing_hint = None
            return
        except Exception as exc:  # noqa: BLE001 - what bleak raises for "insufficient authentication" differs per platform
            log.info("buddy wants a paired link (%r)", exc)
        if attempt == 0:
            STATE.pairing_hint = PAIRING_HINT
            try:
                await client.pair()
            except NotImplementedError:
                pass  # macOS pairs by itself on the retry
            except Exception as exc:  # noqa: BLE001
                log.warning("pairing did not work: %r", exc)
            await asyncio.sleep(1)
    raise PairingNeeded(PAIRING_HINT)


async def ble_loop(address, queue, settings_queue, name=None):
    from bleak import BleakClient

    while True:
        if UPDATE.busy:  # the updater has its own connection to the buddy
            await asyncio.sleep(1)
            continue
        try:
            if address:
                device = address
            else:
                log.info("Scanning for the buddy%s...", f" {name}" if name else "")
                device = await find_buddy(name)
                if device is None:
                    continue
            async with BleakClient(device) as client:
                log.info("BLE connected to %s", getattr(device, "address", device))
                STATE.ble = True
                STATE.buddy = getattr(device, "name", None) or getattr(device, "address", None) or str(device)
                try:
                    version = bytes(await client.read_gatt_char(VER_UUID))
                except Exception:  # noqa: BLE001 - older firmware has no version characteristic
                    version = None
                await secure_link(client, version)
                ok, message = describe_firmware(version)
                (log.info if ok else log.warning)("buddy: %s", message)
                if version and len(version) >= 3:
                    STATE.protocol, STATE.firmware = version[0], firmware_version(version)
                STATE.address = getattr(device, "address", device)
                STATE.can_update = bool(version and len(version) >= 4 and version[3] & SMP_UPDATE_CAP)
                STATE.can_measure = bool(version and len(version) >= 4 and version[3] & 8)
                if STATE.can_measure:
                    try:
                        STATE.set_measure(*unpack_measure(bytes(await client.read_gatt_char(MEASURE_UUID))))
                    except Exception as exc:  # noqa: BLE001
                        log.warning("measurement mode is not available (%r)", exc)
                # Firmware 0.7 and 0.8 before the capability bit existed accepted simulated readings.
                STATE.can_simulate_battery = bool(version and len(version) >= 4 and version[3] & 2) or (
                    version is not None and len(version) == 3 and (version[1], version[2]) >= (0, 7))
                sessions_supported = version is not None and len(version) >= 3 and version[0] >= 2
                if sessions_supported:
                    try:
                        STATE.settings = unpack_settings(bytes(await client.read_gatt_char(CFG_UUID)))
                        STATE.set_status(bytes(await client.read_gatt_char(STATUS_UUID)))
                        await client.start_notify(STATUS_UUID, lambda _c, data: STATE.set_status(bytes(data)))
                    except Exception as exc:  # noqa: BLE001
                        log.warning("settings and status are not available on this firmware (%r)", exc)
                fw = (version[1], version[2]) if version and len(version) >= 3 else (0, 0)
                if fw >= (0, 7):
                    try:
                        STATE.set_battery(bytes(await client.read_gatt_char(BATTERY_UUID)))
                        await client.start_notify(BATTERY_UUID, lambda _c, data: STATE.set_battery(bytes(data)))
                    except Exception as exc:  # noqa: BLE001
                        log.warning("battery status is not available (%r)", exc)
                usage_supported, usage_amounts, time_supported = fw >= (0, 4), fw >= (0, 5), fw >= (0, 6)
                sent_usage, time_sent_at, log_fetched_at = None, None, None
                log_supported = bool(version and len(version) >= 4 and version[3] & 4) and BATTERY_LOG.path is not None
                while not queue.empty():  # events from while we were away are stale
                    queue.get_nowait()
                while client.is_connected:
                    if UPDATE.state == "releasing":
                        break  # the update connects to the buddy itself; leaving the context manager disconnects
                    try:
                        kind, outgoing = settings_queue.get_nowait()
                    except asyncio.QueueEmpty:
                        kind = outgoing = None
                    if kind == "settings":
                        await client.write_gatt_char(CFG_UUID, outgoing, response=True)
                        STATE.settings = unpack_settings(bytes(await client.read_gatt_char(CFG_UUID)))
                        log.info("settings written")
                        continue
                    if kind == "measure":
                        try:
                            await client.write_gatt_char(MEASURE_UUID, outgoing, response=True)
                            STATE.set_measure(*unpack_measure(bytes(await client.read_gatt_char(MEASURE_UUID))))
                            log.info("measurement mode %s", MEASURE_MODES[outgoing[1]])
                        except Exception as exc:  # noqa: BLE001
                            log.warning("the buddy refused the measurement mode (%r)", exc)
                        continue
                    if kind == "battery":
                        try:
                            await client.write_gatt_char(BATTERY_UUID, outgoing, response=True)
                            log.info("battery test value written")
                        except Exception as exc:  # noqa: BLE001 - a firmware with a real gauge refuses this
                            log.warning("the buddy refused the battery test value (%r): its firmware may not accept "
                                        "simulated readings", exc)
                        continue
                    if time_supported and (time_sent_at is None or time.monotonic() - time_sent_at > TIME_REFRESH_S):
                        await client.write_gatt_char(TIME_UUID, time_wire(), response=True)
                        time_sent_at = time.monotonic()
                    if log_supported and (log_fetched_at is None or time.monotonic() - log_fetched_at > LOG_REFRESH_S):
                        log_fetched_at = time.monotonic()
                        try:
                            added = await fetch_battery_log(client, BATTERY_LOG)
                            if added:
                                log.info("battery log: %d new hourly record(s) written to %s", added, BATTERY_LOG.path)
                        except Exception as exc:  # noqa: BLE001 - the log is a nicety; never drop the link over it
                            log.warning("cannot read the battery log (%r)", exc)
                    wire = USAGE.wire(usage_amounts) if usage_supported else None
                    if wire is not None and wire != sent_usage:
                        await client.write_gatt_char(USAGE_UUID, wire, response=True)
                        sent_usage = wire
                        log.info("usage written %s", wire.hex())
                    try:
                        code, slot = await asyncio.wait_for(queue.get(), timeout=0.25)
                    except asyncio.TimeoutError:
                        continue
                    payload = bytes([code, slot]) if sessions_supported else bytes([code])
                    await client.write_gatt_char(EVT_UUID, payload, response=True)
                    log.info("wrote %s", payload.hex())
            log.warning("BLE disconnected")
        except PairingNeeded as exc:
            log.warning("%s", exc)
            await asyncio.sleep(10)  # the buddy's pairing window is two minutes: keep trying, but gently
        except Exception as exc:  # noqa: BLE001 - the bridge must keep running
            log.error("BLE error: %r", exc)
            await asyncio.sleep(3)
        finally:
            STATE.reset_link()


# A tiny stand-in for the buddy: lets the web page be tried (and tested) without hardware.
SIM_MOOD_FOR_EVENT = {"turn_finished": 4, "error": 6}


async def simulated_ble_loop(queue, settings_queue):
    STATE.ble, STATE.simulated = True, True
    STATE.firmware, STATE.protocol = "0.3 (simulated)", PROTOCOL_VERSION
    STATE.settings = dict(SETTINGS_DEFAULTS)
    STATE.can_update = True
    STATE.can_simulate_battery = True
    STATE.can_measure = True
    names = {v: k for k, v in EVENT_CODES.items()}
    sessions = {}  # slot -> "working" | "waiting" | "idle"
    limit = False

    def publish(event_mood=None):
        working = sum(1 for v in sessions.values() if v == "working")
        waiting = sum(1 for v in sessions.values() if v == "waiting")
        mood = 8 if limit else 5 if waiting else event_mood if event_mood else 3 if working else 1
        flags = (2 if STATE.settings["muted"] else 0) | (4 if STATE.measure_mode else 0) | (
            1 if STATE.measure_mode == "display_off" else 0)
        STATE.set_status(bytes([mood, len(sessions), working, waiting, flags]))

    publish()
    log.info("Simulated buddy ready (no Bluetooth is used)")
    while True:
        while not settings_queue.empty():
            kind, outgoing = settings_queue.get_nowait()
            if kind == "measure":
                mode = MEASURE_MODES[outgoing[1]]
                if mode != "beep":
                    STATE.set_measure(mode, 30)
                log.info("(simulated) measurement mode %s", mode)
                publish()
                continue
            if kind == "battery":
                STATE.battery = unpack_battery(outgoing)
                log.info("(simulated) battery test value written")
                continue
            STATE.settings = unpack_settings(outgoing)
            log.info("(simulated) settings written")
            publish()
        try:
            code, slot = await asyncio.wait_for(queue.get(), timeout=0.25)
        except asyncio.TimeoutError:
            continue
        name = names.get(code)
        if name == "session_ended":
            sessions.pop(slot, None)
        elif name == "session_started":
            sessions[slot] = "idle"
        elif name in ("prompt_submitted", "answered"):
            sessions[slot] = "working"
            limit = limit and name == "answered"
        elif name == "needs_input":
            sessions[slot] = "waiting"
        elif name in ("turn_finished", "idle"):
            sessions[slot] = "idle"
        elif name == "limit_reached":
            limit = True
            sessions = {k: "idle" for k in sessions}
        else:
            sessions.setdefault(slot, "idle")
        publish(SIM_MOOD_FOR_EVENT.get(name))
        log.info("(simulated) would write 0x%02x for slot %d", code, slot)


async def no_ble_loop(queue):
    while True:
        code, slot = await queue.get()
        log.info("(BLE disabled) would write 0x%02x for slot %d", code, slot)


# ---------------------------------------------------------------- main


async def amain(args):
    cfg = load_config(args.config)
    loop = asyncio.get_running_loop()
    queue = asyncio.Queue()
    settings_queue = asyncio.Queue()
    bridge = cfg.get("bridge", {})
    inputs = bridge.get("inputs", ["http"])
    BATTERY_LOG.configure(cfg.get("battery_log_file") or os.path.join(os.path.expanduser("~"), ".local", "share", "claude-buddy", "battery-log.csv"))
    USAGE.path = cfg.get("usage_file") or os.path.join(os.path.expanduser("~"), ".local", "share", "claude-buddy", "usage.json")
    USAGE.load()
    keep = []  # strong references so tasks and servers are not garbage collected

    async def flush_usage():
        while True:
            await asyncio.sleep(USAGE.SAVE_EVERY + 1)
            USAGE.save()

    keep.append(asyncio.create_task(flush_usage()))

    if "http" in inputs or bridge.get("web", True):
        keep.append(await start_http(cfg.get("http_port", 8787), queue, settings_queue))

    tunnel = bridge.get("ssh_tunnel", {})
    host_override = port_override = None
    if tunnel.get("enabled", False):
        keep.append(asyncio.create_task(run_tunnel(tunnel)))
        host_override, port_override = "127.0.0.1", int(tunnel.get("local_port", 11883))

    if "mqtt" in inputs:
        keep.append(start_mqtt(cfg.get("relay", {}), queue, loop, host_override, port_override))

    if args.simulate:
        await simulated_ble_loop(queue, settings_queue)
    elif args.no_ble or not bridge.get("ble", True):
        await no_ble_loop(queue)
    else:
        await ble_loop(bridge.get("ble_address"), queue, settings_queue, bridge.get("ble_name"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--config", default=default_config_path())
    ap.add_argument("--no-ble", action="store_true", help="log events only (testing)")
    ap.add_argument("--simulate", action="store_true", help="pretend to be connected to a buddy (try the web page)")
    ap.add_argument("--log-file", help="write a rotating log file (use with pythonw)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    handlers = []
    if args.log_file:
        os.makedirs(os.path.dirname(os.path.abspath(args.log_file)), exist_ok=True)
        handlers.append(logging.handlers.RotatingFileHandler(args.log_file, maxBytes=1_000_000, backupCount=3))
    elif sys.stderr is not None:
        handlers.append(logging.StreamHandler())
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        handlers=handlers,
    )
    try:
        asyncio.run(amain(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
