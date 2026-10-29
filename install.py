#!/usr/bin/env python3
"""Claude Buddy installer (Linux, macOS, Windows). Standard library only.

    python3 install.py              interactive install
    python3 install.py --status     show what is installed and running
    python3 install.py --doctor     check every part and explain what is wrong
    python3 install.py --uninstall  remove hooks and services (add --purge to also delete config)

The installer asks where Claude Code runs and where the buddy sits, then sets up only what that
needs. The default (Claude Code and the buddy on the same machine) uses no broker at all.
"""

import argparse
import getpass
import json
import os
import platform
import secrets
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent
HOME = Path.home()
CONFIG_DIR = HOME / ".config" / "claude-buddy"
CONFIG = CONFIG_DIR / "config.json"
DATA_DIR = HOME / ".local" / "share" / "claude-buddy"
SETTINGS = HOME / ".claude" / "settings.json"

SYSTEM = platform.system()  # Linux, Darwin, Windows
SERVICE_NAME = "claude-buddy-bridge"
TASK_NAME = "ClaudeBuddyBridge"
LAUNCHD_LABEL = "io.github.claude-buddy.bridge"
BROKER_CONTAINER = "claude-buddy-mqtt"
BROKER_VOLUMES = ("claude-buddy-mqtt-config", "claude-buddy-mqtt-data")
BROKER_IMAGE = "eclipse-mosquitto:2"
DEFAULT_HTTP_PORT = 8787
DEFAULT_TOPIC = "claude-buddy/events"

# (Claude Code hook event, matcher, buddy event)
HOOKS = [
    ("UserPromptSubmit", "", "prompt_submitted"),
    ("Stop", "", "turn_finished"),
    ("PermissionRequest", "", "needs_input"),
    ("PreToolUse", "AskUserQuestion|ExitPlanMode", "needs_input"),
    ("PostToolUse", "", "answered"),
    ("Notification", "idle_prompt", "idle"),
    ("Notification", "permission_prompt", "needs_input"),
    ("PostToolUseFailure", "*", "error"),
    ("StopFailure", "rate_limit", "limit_reached"),
    ("StopFailure",
     "overloaded|authentication_failed|billing_error|oauth_org_not_allowed|invalid_request"
     "|model_not_found|server_error|max_output_tokens|unknown", "error"),
    ("SessionStart", "", "session_started"),
    ("SessionEnd", "", "session_ended"),
]


# ------------------------------------------------------------------ small helpers


def say(msg=""):
    print(msg, flush=True)


def die(msg):
    print(f"\nERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def run(cmd, check=True, quiet=False, **kw):
    if not quiet:
        say("  $ " + " ".join(str(c) for c in cmd))
    return subprocess.run([str(c) for c in cmd], check=check, **kw)


def run_limited(cmd, seconds=20):
    """Runs a command for the doctor: never reads input and never runs longer than `seconds`.
    Returns (returncode, stdout, stderr); returncode is None if it timed out."""
    try:
        r = subprocess.run([str(c) for c in cmd], stdin=subprocess.DEVNULL, capture_output=True,
                           text=True, timeout=seconds)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return None, "", ""
    except OSError as exc:
        return 127, "", str(exc)


def have(tool):
    return shutil.which(tool) is not None


class Asker:
    """Asks questions; with --yes (or a flag given on the command line) it does not block."""

    def __init__(self, assume_yes):
        self.assume_yes = assume_yes

    def yesno(self, question, default, preset=None):
        if preset is not None:
            return preset
        if self.assume_yes:
            return default
        suffix = " [Y/n] " if default else " [y/N] "
        while True:
            answer = input(question + suffix).strip().lower()
            if not answer:
                return default
            if answer in ("y", "yes"):
                return True
            if answer in ("n", "no"):
                return False

    def text(self, question, default=None, preset=None, secret=False):
        if preset is not None:
            return preset
        if self.assume_yes:
            if default is None:
                die(f"'{question}' has no default; pass it as a command-line option.")
            return default
        label = f"{question}" + (f" [{default}]" if default else "") + ": "
        value = (getpass.getpass(label) if secret else input(label)).strip()
        return value or default


# ------------------------------------------------------------------ hooks in Claude Code settings


def hook_command(event):
    py = Path(sys.executable).as_posix()
    emit = (DATA_DIR / "emit.py").as_posix()
    return f'"{py}" "{emit}" {event}'


def is_ours(hook):
    return "claude-buddy" in hook.get("command", "") and "emit.py" in hook.get("command", "")


def load_settings():
    if SETTINGS.exists():
        try:
            return json.loads(SETTINGS.read_text(encoding="utf-8"))
        except ValueError:
            die(f"{SETTINGS} is not valid JSON; fix it first (nothing was changed).")
    return {}


def strip_our_hooks(settings):
    hooks = settings.get("hooks", {})
    for name in list(hooks):
        groups = []
        for group in hooks[name]:
            group["hooks"] = [h for h in group.get("hooks", []) if not is_ours(h)]
            if group["hooks"]:
                groups.append(group)
        if groups:
            hooks[name] = groups
        else:
            del hooks[name]
    if not hooks:
        settings.pop("hooks", None)


def install_hooks():
    settings = load_settings()
    if SETTINGS.exists():
        backup = SETTINGS.with_name(f"settings.json.claude-buddy-{time.strftime('%Y%m%d-%H%M%S')}.bak")
        shutil.copy2(SETTINGS, backup)
        say(f"  backed up {SETTINGS.name} -> {backup.name}")
    strip_our_hooks(settings)
    hooks = settings.setdefault("hooks", {})
    for name, matcher, event in HOOKS:
        hooks.setdefault(name, []).append({
            "matcher": matcher,
            "hooks": [{"type": "command", "command": hook_command(event), "async": True, "timeout": 5}],
        })
    SETTINGS.parent.mkdir(parents=True, exist_ok=True)
    SETTINGS.write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")
    say(f"  installed {len(HOOKS)} hooks in {SETTINGS}")


def remove_hooks():
    if not SETTINGS.exists():
        return
    settings = load_settings()
    strip_our_hooks(settings)
    SETTINGS.write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")
    say(f"  removed Claude Buddy hooks from {SETTINGS}")


def statusline_command():
    return f'"{Path(sys.executable).as_posix()}" "{(DATA_DIR / "statusline.py").as_posix()}"'


def is_our_statusline(entry):
    cmd = (entry or {}).get("command", "")
    return "claude-buddy" in cmd and "statusline.py" in cmd


def previous_spending():
    try:
        return bool((json.loads(CONFIG.read_text(encoding="utf-8")).get("statusline") or {}).get("enabled"))
    except (OSError, ValueError):
        return False


def previous_chain():
    """The user's own status line command remembered by an earlier run, if any."""
    try:
        return (json.loads(CONFIG.read_text(encoding="utf-8")).get("statusline") or {}).get("chain")
    except (OSError, ValueError):
        return None


def install_statusline():
    """Makes our command the status line; the user's own one is kept as 'chain' and still shown. Returns the chain."""
    settings = load_settings()
    current = settings.get("statusLine")
    chain = previous_chain()
    if current and not is_our_statusline(current):
        if current.get("type") == "command" and current.get("command"):
            chain = current["command"]
        else:
            say("  (your current status line is not a command, so it is left alone and spending is not forwarded)")
            return previous_chain()
    settings["statusLine"] = {"type": "command", "command": statusline_command()}
    SETTINGS.parent.mkdir(parents=True, exist_ok=True)
    SETTINGS.write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")
    say(f"  status line now forwards spending to the buddy" + (" (your own status line still runs after it)" if chain else ""))
    return chain


def remove_statusline():
    if not SETTINGS.exists():
        return
    settings = load_settings()
    if not is_our_statusline(settings.get("statusLine")):
        return
    chain = previous_chain()
    if chain:
        settings["statusLine"] = {"type": "command", "command": chain}
    else:
        settings.pop("statusLine", None)
    SETTINGS.write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")
    say(f"  restored your status line in {SETTINGS}")


# ------------------------------------------------------------------ files, config, venv


def copy_files():
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    shutil.copy2(REPO / "hooks" / "emit.py", DATA_DIR / "emit.py")
    shutil.copy2(REPO / "hooks" / "statusline.py", DATA_DIR / "statusline.py")
    shutil.copy2(REPO / "bridge" / "claude_buddy_bridge.py", DATA_DIR / "bridge.py")
    shutil.copy2(REPO / "bridge" / "requirements.txt", DATA_DIR / "requirements.txt")
    (DATA_DIR / "web").mkdir(exist_ok=True)
    shutil.copy2(REPO / "bridge" / "web" / "index.html", DATA_DIR / "web" / "index.html")


def write_config(cfg):
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    CONFIG.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")
    try:
        CONFIG.chmod(0o600)  # holds the relay password
    except OSError:
        pass
    say(f"  wrote {CONFIG}")


def venv_python():
    sub = "Scripts/python.exe" if SYSTEM == "Windows" else "bin/python"
    return DATA_DIR / "venv" / sub


def setup_venv():
    if not venv_python().exists():
        run([sys.executable, "-m", "venv", DATA_DIR / "venv"])
    try:
        run([venv_python(), "-m", "pip", "install", "--quiet", "-r", DATA_DIR / "requirements.txt"])
    except subprocess.CalledProcessError:
        die("Installing the Python dependencies failed. The bridge needs 'bleak' and 'paho-mqtt'.\n"
            "       Check your internet access, and that Python has SSL support, then re-run.")


# ------------------------------------------------------------------ MQTT broker (relay host)


def setup_broker(port, user, password):
    """Runs Mosquitto in Docker with a password file. Config and data live in named Docker volumes, so
    the files are owned and protected by the container (no host path or permission problems)."""
    if not have("docker"):
        say("  Docker was not found, so I can't start the relay for you.")
        say("  Install Docker (https://docs.docker.com/get-docker/) and re-run this installer,")
        say("  or run any MQTT broker yourself and choose 'connect to an existing relay'.")
        die("relay not started")
    conf = ("listener 1883\nallow_anonymous false\npassword_file /mosquitto/config/passwd\n"
            "persistence true\npersistence_location /mosquitto/data/\n")
    config_vol, data_vol = BROKER_VOLUMES
    for vol in BROKER_VOLUMES:
        run(["docker", "volume", "create", vol], quiet=True, stdout=subprocess.DEVNULL)
    script = ("cat > /mosquitto/config/mosquitto.conf && rm -f /mosquitto/config/passwd && "  # -c fails on an existing file
              'mosquitto_passwd -b -c /mosquitto/config/passwd "$BUDDY_USER" "$BUDDY_PASSWORD" && '
              "chown mosquitto:mosquitto /mosquitto/config/passwd && chmod 0700 /mosquitto/config/passwd")
    run(["docker", "run", "--rm", "-i", "-e", f"BUDDY_USER={user}", "-e", f"BUDDY_PASSWORD={password}",
         "-v", f"{config_vol}:/mosquitto/config", BROKER_IMAGE, "sh", "-c", script],
        quiet=True, input=conf.encode(), stderr=subprocess.DEVNULL)
    run(["docker", "rm", "-f", BROKER_CONTAINER], check=False, quiet=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run(["docker", "run", "-d", "--name", BROKER_CONTAINER, "--restart", "unless-stopped",
         "-p", f"{port}:1883", "-v", f"{config_vol}:/mosquitto/config", "-v", f"{data_vol}:/mosquitto/data",
         BROKER_IMAGE], quiet=True, stdout=subprocess.DEVNULL)
    say(f"  relay running (container '{BROKER_CONTAINER}', port {port})")


def existing_relay_password():
    """The password of a relay this machine already hosts, so re-running the installer never locks
    out bridges that are already connected."""
    try:
        cfg = json.loads(CONFIG.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if cfg.get("roles", {}).get("relay") == "host" or (
            "roles" not in cfg and (cfg.get("relay") or {}).get("host") in ("localhost", "127.0.0.1")):
        return (cfg.get("relay") or {}).get("password") or None
    return None


# ------------------------------------------------------------------ autostart of the bridge


def bridge_cmd(extra=()):
    return [str(venv_python()), str(DATA_DIR / "bridge.py"), "--config", str(CONFIG), *extra]


def install_service():
    if SYSTEM == "Linux":
        unit_dir = HOME / ".config" / "systemd" / "user"
        unit_dir.mkdir(parents=True, exist_ok=True)
        (unit_dir / f"{SERVICE_NAME}.service").write_text(
            "[Unit]\nDescription=Claude Buddy bridge (Claude Code events to BLE)\n"
            "After=network-online.target bluetooth.target\n\n[Service]\n"
            f"ExecStart={' '.join(bridge_cmd())}\nRestart=always\nRestartSec=5\n\n"
            "[Install]\nWantedBy=default.target\n", encoding="utf-8")
        run(["systemctl", "--user", "daemon-reload"])
        run(["systemctl", "--user", "enable", "--now", f"{SERVICE_NAME}.service"])
        run(["loginctl", "enable-linger", os.environ.get("USER", "")], check=False, quiet=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        say("  bridge will start at boot (systemd user service).")
        say(f"  logs: journalctl --user -u {SERVICE_NAME} -f")
    elif SYSTEM == "Darwin":
        plist = HOME / "Library" / "LaunchAgents" / f"{LAUNCHD_LABEL}.plist"
        plist.parent.mkdir(parents=True, exist_ok=True)
        args = "".join(f"<string>{a}</string>" for a in bridge_cmd())
        log = DATA_DIR / "bridge.log"
        plist.write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
            '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0"><dict>'
            f"<key>Label</key><string>{LAUNCHD_LABEL}</string>"
            f"<key>ProgramArguments</key><array>{args}</array>"
            "<key>RunAtLoad</key><true/><key>KeepAlive</key><true/>"
            f"<key>StandardErrorPath</key><string>{log}</string>"
            f"<key>StandardOutPath</key><string>{log}</string></dict></plist>\n", encoding="utf-8")
        run(["launchctl", "unload", plist], check=False, quiet=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        run(["launchctl", "load", "-w", plist])
        say(f"  bridge will start at login (launchd). Logs: {log}")
        say("  macOS may ask to allow Bluetooth for Python the first time.")
    elif SYSTEM == "Windows":
        pythonw = venv_python().with_name("pythonw.exe")
        log = DATA_DIR / "bridge.log"
        args = f'"{DATA_DIR / "bridge.py"}" --config "{CONFIG}" --log-file "{log}"'
        ps = (
            f"$a = New-ScheduledTaskAction -Execute '{pythonw}' -Argument '{args}'; "
            "$t = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME; "
            "$s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries "
            "-StartWhenAvailable -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) "
            "-ExecutionTimeLimit ([TimeSpan]::Zero); "
            f"Register-ScheduledTask -TaskName '{TASK_NAME}' -Action $a -Trigger $t -Settings $s -Force | Out-Null; "
            f"Stop-ScheduledTask -TaskName '{TASK_NAME}' -ErrorAction SilentlyContinue; "
            + WIN_KILL_BRIDGES +  # Stop-ScheduledTask can leave the old bridge running and holding the port
            f"Start-ScheduledTask -TaskName '{TASK_NAME}'"
        )
        run(["powershell", "-NoProfile", "-Command", ps], quiet=True)
        say(f"  bridge will start at logon (scheduled task '{TASK_NAME}'). Logs: {log}")
    else:
        say(f"  Unknown OS '{SYSTEM}': start the bridge yourself: {' '.join(bridge_cmd())}")


WIN_KILL_BRIDGES = (
    "Get-CimInstance Win32_Process -Filter \"Name like 'python%'\" | "
    "Where-Object { $_.CommandLine -like '*claude-buddy*bridge.py*' } | "
    "ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }; "
)


def remove_service():
    if SYSTEM == "Linux":
        run(["systemctl", "--user", "disable", "--now", f"{SERVICE_NAME}.service"], check=False, quiet=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        (HOME / ".config" / "systemd" / "user" / f"{SERVICE_NAME}.service").unlink(missing_ok=True)
    elif SYSTEM == "Darwin":
        plist = HOME / "Library" / "LaunchAgents" / f"{LAUNCHD_LABEL}.plist"
        run(["launchctl", "unload", plist], check=False, quiet=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        plist.unlink(missing_ok=True)
    elif SYSTEM == "Windows":
        run(["powershell", "-NoProfile", "-Command",
             f"Stop-ScheduledTask -TaskName '{TASK_NAME}' -ErrorAction SilentlyContinue; " + WIN_KILL_BRIDGES +
             f"Unregister-ScheduledTask -TaskName '{TASK_NAME}' -Confirm:$false -ErrorAction SilentlyContinue"],
            check=False, quiet=True)


# ------------------------------------------------------------------ commands


def cmd_status():
    say(f"Config:  {CONFIG} ({'present' if CONFIG.exists() else 'missing'})")
    if CONFIG.exists():
        cfg = json.loads(CONFIG.read_text(encoding="utf-8"))
        say(f"  hooks send via: {cfg.get('hook_transport')}   bridge inputs: {cfg.get('bridge', {}).get('inputs')}")
    settings = load_settings() if SETTINGS.exists() else {}
    n = sum(1 for g in settings.get("hooks", {}).values() for grp in g for h in grp.get("hooks", []) if is_ours(h))
    say(f"Hooks:   {n} Claude Buddy hooks in {SETTINGS}")
    say(f"Bridge:  {service_state()}")
    if have("docker"):
        _, out, _ = run_limited(["docker", "ps", "--filter", f"name={BROKER_CONTAINER}", "--format", "{{.Status}}"])
        if out.strip():
            say(f"Relay:   {out.strip()} (container {BROKER_CONTAINER})")


def cmd_uninstall(purge):
    say("Removing Claude Buddy...")
    remove_hooks()
    remove_statusline()
    remove_service()
    if purge:
        if have("docker"):
            run(["docker", "rm", "-f", BROKER_CONTAINER], check=False, quiet=True,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for vol in BROKER_VOLUMES:
                run(["docker", "volume", "rm", vol], check=False, quiet=True,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        shutil.rmtree(DATA_DIR, ignore_errors=True)
        shutil.rmtree(CONFIG_DIR, ignore_errors=True)
        say("  deleted config, data and the relay container.")
    else:
        say(f"  kept {CONFIG_DIR} and {DATA_DIR} (use --purge to delete them too).")


INTRO = """\
Claude Buddy shows what Claude Code is doing on a small Bluetooth gadget (the "buddy").

Two programs work together:
  * Claude Code sends a "hook" message whenever something happens (you ask, it finishes, it needs you).
  * The bridge receives those messages and sends them to the buddy over Bluetooth.
"""

MENU = """\
How do you want to set it up?

  1) Everything on this computer                                        (recommended)
       Claude Code and the buddy are both here. Nothing else to set up.

         Claude Code --> bridge --Bluetooth--> buddy          (all on this computer)

  2) Claude Code is on ANOTHER machine; the buddy sits next to THIS computer
       For example a remote dev box, and your laptop on the desk. Needs a "relay"
       (a small message server) that the other machine hosts.

         Claude Code (other machine) --> relay --> bridge (here) --Bluetooth--> buddy

  3) Claude Code is on THIS computer; the buddy sits next to ANOTHER one
       This computer hosts the relay. Afterwards, run this installer on the other
       computer and choose 2.

  4) Everything here, plus a second buddy elsewhere (home office, another desk)
       Like 1, and this computer also hosts the relay that the second buddy's
       computer will connect to (it chooses 2).
"""


def choose_setup(ask, args):
    """Returns (claude, buddy, relay_role) where relay_role is None, 'host' or 'join'."""
    if args.claude is not None or args.buddy is not None or args.relay is not None:
        claude = True if args.claude is None else args.claude
        buddy = True if args.buddy is None else args.buddy
        if claude and buddy:
            role = "host" if args.relay in ("host",) else None
        elif claude:
            role = "join" if args.relay == "join" else "host"
        else:
            role = "join"
        return claude, buddy, role
    say(INTRO)
    if ask.assume_yes:
        return True, True, None
    say(MENU)
    while True:
        choice = input("Choice [1]: ").strip() or "1"
        if choice in ("1", "2", "3", "4"):
            break
    say("")
    if choice == "1":
        return True, True, None
    if choice == "2":
        return False, True, "join"
    if choice == "3":
        join = ask.yesno("Use a relay that already exists instead of hosting one here?", False)
        return True, False, "join" if join else "host"
    return True, True, "host"


def describe_plan(claude, buddy, relay_role, relay, tunnel, spending=False):
    steps = []
    if claude:
        steps.append(f"Add {len(HOOKS)} hooks to {SETTINGS} (a backup is made first; your own hooks are kept).")
    if claude and spending:
        steps.append(f"Make Claude Code's status line forward its estimated cost to the buddy (any status line you have keeps working).")
    if relay_role == "host":
        steps.append(f"Start a relay (Docker container '{BROKER_CONTAINER}') on port {relay['port']}.")
    if relay_role == "join":
        how = f"through an SSH tunnel to {tunnel['target']}" if tunnel.get("enabled") else "directly"
        steps.append(f"Connect to the relay at {relay['host']}:{relay['port']} {how}.")
    if buddy:
        start = {"Linux": "a systemd user service", "Darwin": "a launchd agent",
                 "Windows": "a scheduled task"}.get(SYSTEM, "a background process")
        steps.append(f"Install the bridge (with its own Python environment) and start it at login as {start}.")
    steps.append(f"Save your choices in {CONFIG} and program files in {DATA_DIR}.")
    return steps


def cmd_install(args):
    ask = Asker(args.yes)
    say("Claude Buddy installer")
    say("======================\n")
    if sys.version_info < (3, 9):
        die("Python 3.9 or newer is required.")

    claude, buddy, relay_role = choose_setup(ask, args)
    if not claude and not buddy:
        die("Nothing to install: this machine neither runs Claude Code nor hosts a buddy.")

    relay = {"host": "localhost", "port": 1883, "user": "claude-buddy", "password": "", "topic": DEFAULT_TOPIC}
    tunnel = {"enabled": False}
    if relay_role == "host":
        relay["port"] = int(ask.text("Port for the relay", "1883", args.relay_port))
        reused = existing_relay_password()
        relay["password"] = reused or secrets.token_urlsafe(18)
        if reused:
            say("  (keeping the relay password from the previous installation, so connected bridges keep working)")
    elif relay_role == "join":
        say("To reach the relay you need its address and the password that was generated when it was set up")
        say("(it is under \"relay\" in ~/.config/claude-buddy/config.json on the machine that hosts it).\n")
        relay["host"] = ask.text("Relay address (host name or IP)", None, args.relay_host)
        relay["port"] = int(ask.text("Relay port", "1883", args.relay_port))
        relay["user"] = ask.text("Relay user", relay["user"], args.relay_user)
        relay["password"] = ask.text("Relay password", None, args.relay_password, secret=True)
        say("\nSome networks (for example corporate VPNs) reset plain connections to the relay. An SSH tunnel avoids that.")
        say("It needs key-based SSH login to the machine that hosts the relay (no password prompt).")
        if ask.yesno("Reach the relay through an SSH tunnel?", False, None if args.ssh_tunnel is None else True):
            target = ask.text("SSH target, as user@host", None, args.ssh_tunnel)
            tunnel = {"enabled": True, "target": target, "local_port": 11883, "remote_port": relay["port"]}

    spending = False
    if claude:
        say("\nOptional: the buddy can track what your Claude Code usage would cost at list prices (an estimate) and")
        say("warn when you pass a monthly budget. It works by making Claude Code's status line send the cost to the bridge;")
        say("a status line you already have keeps working.")
        spending = ask.yesno("Track estimated spending?", previous_spending(), args.spending)

    use_relay = relay_role is not None
    cfg = {
        "version": 1,
        "roles": {"claude": claude, "buddy": buddy, "relay": relay_role or "none"},
        "hook_transport": "mqtt" if (claude and use_relay) else "http",
        "http_port": DEFAULT_HTTP_PORT,
        "relay": relay if use_relay else {},
        "bridge": {"inputs": ["mqtt"] if use_relay else ["http"], "ble": True, "ble_address": None, "ble_name": None,
                   "ssh_tunnel": tunnel},
        "statusline": {"enabled": spending},
    }

    say("\nThis is what I am going to do:")
    for i, step in enumerate(describe_plan(claude, buddy, relay_role, relay, tunnel, spending), 1):
        say(f"  {i}. {step}")
    if not ask.yesno("\nGo ahead?", True):
        say("Nothing was changed.")
        return

    say("\nInstalling...")
    if relay_role == "host":
        setup_broker(relay["port"], relay["user"], relay["password"])
    copy_files()
    if claude:
        install_hooks()  # first, so its backup of settings.json is the untouched original
        if spending:
            chain = install_statusline()
            if chain:
                cfg["statusline"]["chain"] = chain
        else:
            remove_statusline()  # opted out (or never in): give back the user's own status line
    write_config(cfg)
    if buddy:
        setup_venv()
        if not args.no_service:
            install_service()
        else:
            say(f"  skipped autostart. Run it yourself: {' '.join(bridge_cmd())}")

    say("\nDone. Next steps:")
    n = 1
    if buddy:
        say(f"  {n}. Power the buddy. The bridge finds it by itself within a few seconds.")
        n += 1
    if claude:
        say(f"  {n}. Restart any running Claude Code session so it picks up the new hooks.")
        n += 1
        say(f"  {n}. Test: {sys.executable} {DATA_DIR / 'emit.py'} prompt_submitted   (the eyes should start sweeping)")
        n += 1
    if buddy:
        say(f"  {n}. Open http://127.0.0.1:{DEFAULT_HTTP_PORT}/ in a browser for the buddy's status and settings.")
        n += 1
    say(f"  {n}. If something does not work: python3 install.py --doctor")
    if relay_role == "host":
        say(f"\nTo add another machine, run this installer there and choose 2.\n"
            f"  relay address: this machine's name or IP   port: {relay['port']}   user: {relay['user']}\n"
            f"  password: shown under \"relay\" in {CONFIG}")


# ------------------------------------------------------------------ doctor


class Report:
    def __init__(self):
        self.failures = 0
        self.warnings = 0

    def ok(self, msg):
        say(f"  [ OK ] {msg}")

    def warn(self, msg, hint=None):
        self.warnings += 1
        say(f"  [WARN] {msg}")
        if hint:
            say(f"         -> {hint}")

    def fail(self, msg, hint=None):
        self.failures += 1
        say(f"  [FAIL] {msg}")
        if hint:
            say(f"         -> {hint}")


def load_emit_module():
    import importlib.util
    path = REPO / "hooks" / "emit.py"  # this checkout's copy: an older installed one may lack newer helpers
    if not path.exists():
        path = DATA_DIR / "emit.py"
    spec = importlib.util.spec_from_file_location("claude_buddy_emit", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def service_state():
    if SYSTEM == "Linux" and have("systemctl"):
        _, out, _ = run_limited(["systemctl", "--user", "is-active", f"{SERVICE_NAME}.service"])
        return out.strip() or "not installed"
    if SYSTEM == "Windows":
        rc, out, _ = run_limited(["powershell", "-NoProfile", "-Command",
                                  f"(Get-ScheduledTask -TaskName '{TASK_NAME}' -ErrorAction SilentlyContinue).State"], 30)
        return "unknown (timed out)" if rc is None else out.strip() or "not installed"
    if SYSTEM == "Darwin":
        rc, _, _ = run_limited(["launchctl", "list", LAUNCHD_LABEL])
        return "loaded" if rc == 0 else "not installed"
    return "unknown"


def recent_bridge_log(lines=60):
    if SYSTEM == "Linux" and have("journalctl"):
        _, out, _ = run_limited(["journalctl", "--user", "-u", f"{SERVICE_NAME}.service", "-n", str(lines),
                                 "--no-pager", "-o", "cat"])
        return out
    log = DATA_DIR / "bridge.log"
    if log.exists():
        return "\n".join(log.read_text(encoding="utf-8", errors="replace").splitlines()[-lines:])
    return ""


def cmd_doctor():
    import socket
    rep = Report()
    say("Claude Buddy doctor\n===================\n")

    say("Basics")
    if sys.version_info >= (3, 9):
        rep.ok(f"Python {sys.version.split()[0]}")
    else:
        rep.fail(f"Python {sys.version.split()[0]} is too old", "install Python 3.9 or newer")

    say("\nConfiguration")
    if not CONFIG.exists():
        rep.fail(f"no config at {CONFIG}", "run: python3 install.py")
        say(f"\n{rep.failures} problem(s) found.")
        sys.exit(1)
    try:
        cfg = json.loads(CONFIG.read_text(encoding="utf-8"))
    except ValueError:
        rep.fail(f"{CONFIG} is not valid JSON", "fix it, or run install.py again")
        sys.exit(1)
    transport = cfg.get("hook_transport", "http")
    inputs = cfg.get("bridge", {}).get("inputs", [])
    roles = cfg.get("roles", {})
    buddy_here = roles.get("buddy", venv_python().exists())  # older configs: infer from the install
    rep.ok(f"hooks send via {transport}" + (f"; the bridge reads from {', '.join(inputs)}" if buddy_here else ""))
    for name, source in (("emit.py", REPO / "hooks" / "emit.py"),
                         ("bridge.py", REPO / "bridge" / "claude_buddy_bridge.py")):
        installed = DATA_DIR / name
        if not installed.exists():
            rep.fail(f"{name} missing in {DATA_DIR}", "run: python3 install.py")
        elif source.exists() and installed.read_bytes() != source.read_bytes():
            rep.warn(f"installed {name} differs from this checkout (an older or modified copy)",
                     "run: python3 install.py to update it")
        else:
            rep.ok(f"{name} installed")

    say("\nClaude Code hooks")
    settings = load_settings() if SETTINGS.exists() else {}
    ours = [h for g in settings.get("hooks", {}).values() for grp in g for h in grp.get("hooks", []) if is_ours(h)]
    if not ours:
        say("  (none installed on this machine)")
    elif len(ours) == len(HOOKS):
        rep.ok(f"{len(ours)} hooks in {SETTINGS}")
    else:
        rep.warn(f"{len(ours)} of {len(HOOKS)} hooks in {SETTINGS}", "run: python3 install.py to repair")
    if (cfg.get("statusline") or {}).get("enabled"):
        if not is_our_statusline(settings.get("statusLine")):
            rep.warn("spending tracking is on, but Claude Code's status line is not ours any more",
                     "run: python3 install.py --spending yes")
        elif not (DATA_DIR / "statusline.py").exists():
            rep.fail(f"statusline.py missing in {DATA_DIR}", "run: python3 install.py")
        else:
            rep.ok("status line forwards estimated spending" +
                   (" (and runs your own status line)" if cfg["statusline"].get("chain") else ""))
    if ours and not Path(sys.executable).exists():
        rep.fail("the Python used by the hooks no longer exists", "run: python3 install.py")
    if ours:
        say("  (Claude Code reads hooks when a session starts: restart running sessions after installing.)")

    if ours:
        say("\nSending events")
        if transport == "http":
            port = int(cfg.get("http_port", DEFAULT_HTTP_PORT))
            try:
                socket.create_connection(("127.0.0.1", port), timeout=2).close()
                rep.ok(f"the bridge is listening on 127.0.0.1:{port}")
            except OSError:
                rep.fail(f"nothing is listening on 127.0.0.1:{port}", "the bridge is not running; see 'Bridge' below")
        else:
            ok, msg = load_emit_module().probe_mqtt(cfg.get("relay", {}))
            if ok:
                rep.ok(f"relay: {msg}")
            else:
                rep.fail(f"relay: {msg}", "check the relay address and password in " + str(CONFIG) +
                         ", and that the machine hosting the relay is reachable (VPN up?)")

    if buddy_here:
        say("\nBridge")
        state = service_state()
        if state in ("active", "Running", "Ready", "loaded"):
            rep.ok(f"autostart entry is {state.lower()}")
            web_port = int(cfg.get("http_port", DEFAULT_HTTP_PORT))
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{web_port}/api/status", timeout=3) as r:
                    rep.ok(f"web page answers: http://127.0.0.1:{web_port}/ (bridge {json.load(r).get('bridge_version')})")
            except Exception as exc:  # refused, timeout, or an old bridge without /api/status
                rep.fail(f"web page does not answer on 127.0.0.1:{web_port} ({exc})",
                         "the running bridge is old or failed to start: run python3 install.py, then check "
                         f"{DATA_DIR / 'bridge.log'} for 'HTTP and web page listening' or 'address already in use'")
        elif state == "not installed":
            say("  (no bridge installed on this machine)")
        else:
            rep.fail(f"autostart entry is '{state}'", "run: python3 install.py, or start it by hand (see docs/SETUP.md)")
        if venv_python().exists():
            rc, _, _ = run_limited([venv_python(), "-c", "import bleak, paho.mqtt.client"], 60)
            if rc == 0:
                rep.ok("Bluetooth and MQTT libraries are installed")
            else:
                rep.fail("the bridge's Python libraries are broken", "run: python3 install.py")
        elif state != "not installed":
            rep.fail("the bridge's Python environment is missing", "run: python3 install.py")
        tun = cfg.get("bridge", {}).get("ssh_tunnel", {})
        if tun.get("enabled"):
            rc, _, err = run_limited(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
                                      "-o", "StrictHostKeyChecking=accept-new", tun["target"], "true"], 25)
            if rc == 0:
                rep.ok(f"SSH login to {tun['target']} works without a password")
            elif rc is None:
                rep.fail(f"SSH to {tun['target']} did not answer within 25 seconds",
                         f"try by hand: ssh -o BatchMode=yes {tun['target']} true  (is the machine reachable? is the VPN up?)")
            else:
                rep.fail(f"cannot SSH to {tun['target']} without a password prompt",
                         "set up key-based login (ssh-keygen, then copy the public key to that machine)")
        log = recent_bridge_log()
        if log:
            if "BLE connected" in log and log.rfind("BLE connected") > log.rfind("BLE disconnected"):
                rep.ok("the bridge is connected to the buddy")
            elif "Scanning for the buddy" in log:
                rep.warn("the bridge is still looking for the buddy",
                         "power it on and bring it close; make sure no phone app is connected to it")
            if "MQTT connect refused" in log or "SSH tunnel exited" in log:
                rep.warn("the bridge log shows relay or tunnel trouble", "see: python3 install.py --doctor output above, and docs/TROUBLESHOOTING.md")

    relay_here = roles.get("relay") == "host" or (
        "relay" not in roles and (cfg.get("relay") or {}).get("host") in ("localhost", "127.0.0.1"))
    if relay_here and have("docker"):
        say("\nRelay (hosted on this machine)")
        _, out, _ = run_limited(["docker", "ps", "--filter", f"name={BROKER_CONTAINER}", "--format", "{{.Status}}"])
        if out.strip().startswith("Up"):
            rep.ok(f"container {BROKER_CONTAINER}: {out.strip()}")
        else:
            rep.fail(f"container {BROKER_CONTAINER} is not running", f"docker start {BROKER_CONTAINER}, or run: python3 install.py")

    say("")
    if rep.failures:
        say(f"{rep.failures} problem(s) found, {rep.warnings} warning(s). Each problem has a suggested fix above.")
        sys.exit(1)
    say(f"Everything checks out ({rep.warnings} warning(s)).")


def main():
    ap = argparse.ArgumentParser(description="Claude Buddy installer")
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--doctor", action="store_true", help="check every part of the setup and explain problems")
    ap.add_argument("--uninstall", action="store_true")
    ap.add_argument("--purge", action="store_true", help="with --uninstall: also delete config, data, broker")
    ap.add_argument("--yes", action="store_true", help="accept defaults without asking")
    ap.add_argument("--claude", type=lambda v: v.lower() in ("1", "y", "yes", "true"),
                    help="this machine runs Claude Code (yes/no)")
    ap.add_argument("--buddy", type=lambda v: v.lower() in ("1", "y", "yes", "true"),
                    help="the buddy is next to this machine (yes/no)")
    ap.add_argument("--spending", type=lambda v: v.lower() in ("1", "y", "yes", "true"),
                    help="forward estimated cost from Claude Code's status line (yes/no)")
    ap.add_argument("--relay", choices=["none", "host", "join"])
    ap.add_argument("--relay-host")
    ap.add_argument("--relay-port")
    ap.add_argument("--relay-user")
    ap.add_argument("--relay-password")
    ap.add_argument("--ssh-tunnel", metavar="USER@HOST")
    ap.add_argument("--no-service", action="store_true", help="do not set up autostart")
    args = ap.parse_args()

    if args.doctor:
        cmd_doctor()
    elif args.status:
        cmd_status()
    elif args.uninstall:
        cmd_uninstall(args.purge)
    else:
        cmd_install(args)


if __name__ == "__main__":
    main()
