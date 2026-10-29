# Setup

`python3 install.py` (on Windows: `py install.py`) is the only thing you run. It needs Python 3.9+, explains
what it is about to do, and asks before changing anything.

Two programs work together:

* **Claude Code** runs a tiny command (a *hook*) whenever something happens.
* The **bridge** receives those messages and sends them to the buddy over Bluetooth.

If both are on the same computer, that is all there is. If not, a small **relay** carries the messages between machines.

## The four setups

The installer asks you to pick one. **Option 1 is the default.**

```
1) Everything on this computer                                  (recommended)
     Claude Code --> bridge --Bluetooth--> buddy               (all on this computer)

2) Claude Code is on ANOTHER machine; the buddy sits next to THIS computer
     Claude Code (other machine) --> relay --> bridge (here) --Bluetooth--> buddy

3) Claude Code is on THIS computer; the buddy sits next to ANOTHER one
     Claude Code (here) --> relay (here) --> bridge (other computer) --Bluetooth--> buddy

4) Everything here, plus a second buddy elsewhere
     like 1, and this computer also hosts the relay for the second buddy's computer
```

| You want | Run the installer on... | Choose |
|---|---|---|
| One buddy, Claude Code on the same computer | that computer | **1** |
| Claude Code on a remote dev box, buddy on your desk | the remote box, then your desk computer | **3**, then **2** |
| A second buddy at another location that mirrors the first | this computer, then the other one | **4**, then **2** |

### Option 1: everything on this computer

Press Enter at the first question, read the summary, press Enter again. The installer installs the hooks, creates a
Python environment with the Bluetooth library, and registers the bridge to start at login. Power the buddy and you're done.
No broker, nothing exposed to the network: the bridge listens on `127.0.0.1` only.

### Options 3 and 4: hosting the relay

The relay is an MQTT broker (Mosquitto) that runs in **Docker**, so Docker must be installed on this machine.
The installer generates a password and saves it in `~/.config/claude-buddy/config.json`. The other machines need
this machine's address, the port (1883) and that password.

### Option 2: connecting to a relay

You will be asked for the relay's address, port, user and password, then:

> *Reach the relay through an SSH tunnel?*

Answer **yes** if the computer connects to the relay over a VPN or any network that might block or reset plain
connections (symptom: the bridge's log shows the connection dropping immediately; see
[TROUBLESHOOTING.md](TROUBLESHOOTING.md)). The bridge then keeps an `ssh -L` tunnel alive by itself. This needs
**key-based SSH login** to the relay machine (`ssh user@host` must work without a password prompt), and the installer asks
for that `user@host`. Answer **no** on a normal network.

## The web page

Every machine that hosts a buddy serves a small page at **http://127.0.0.1:8787/**:

* the buddy's connection, firmware version, mood and how many Claude sessions are working or waiting for you;
* the **settings**: brightness, how the display behaves while the buddy sleeps (dimmed or off), eye style, buzzer volume,
  mute and quiet hours. Changes are saved on the buddy itself, so they survive a power cycle;
* buttons that send test events, and a log of recent events.

The page works in any browser on any OS. It is only reachable from the machine itself (it listens on `127.0.0.1`), and it
refuses requests that come from other web sites. To reach it from another computer, use an SSH tunnel
(`ssh -L 8787:127.0.0.1:8787 user@machine`).

No buddy at hand? `bridge.py --simulate` pretends to be one, so you can try the page.

## What the installer changes

| Where | What |
|---|---|
| `~/.config/claude-buddy/config.json` | Your answers, and the relay password if any (mode 600) |
| `~/.local/share/claude-buddy/` | The bridge, its web page, `emit.py` and a Python virtual environment |
| `~/.claude/settings.json` | 12 hooks (timestamped backup `settings.json.claude-buddy-*.bak` first; your own hooks are left alone) |
| Autostart | Linux: a systemd *user* service. macOS: a launchd agent. Windows: a scheduled task that runs at logon, even on battery, and restarts on failure |
| Docker | Only when you host a relay: a container named `claude-buddy-mqtt` |

Re-running the installer is safe: it replaces its own hooks and leaves everything else alone.

## When something doesn't work: the doctor

```bash
python3 install.py --doctor
```

It checks each link of the chain on the machine you run it on (Python, configuration, hooks, whether events can be sent,
the bridge and its libraries, the SSH tunnel, the relay, and the bridge's recent log) and prints `[ OK ]`, `[WARN]` or
`[FAIL]` for each, with a suggested fix for every problem. Run it on each machine of a split setup.

```
Sending events
  [FAIL] relay: connection reset right after connecting (a VPN or firewall may be interfering; try the SSH tunnel option)
         -> check the relay address and password in ~/.config/claude-buddy/config.json, and that the machine hosting the relay is reachable
```

Other commands:

```bash
python3 install.py --status                  # what is installed
python3 ~/.local/share/claude-buddy/emit.py prompt_submitted    # send a test event (eyes should sweep)
python3 install.py --uninstall               # remove hooks and autostart (add --purge to delete everything)
```

Logs: Linux `journalctl --user -u claude-buddy-bridge -f`; macOS and Windows `~/.local/share/claude-buddy/bridge.log`.

## Unattended installs

Every question has a command-line option, and `--yes` accepts the defaults; see `python3 install.py --help`.

```bash
python3 install.py --yes --claude no --buddy yes --relay join \
    --relay-host devbox.example.org --relay-user claude-buddy --relay-password-file ~/relay-password.txt \
    --ssh-tunnel me@devbox.example.org
```

The relay password is the one value with no plain option, so it never lands in your shell history or the process list.
Give it as a file (`--relay-password-file <path>`, or `-` to read one line from stdin), or in the environment variable
`CLAUDE_BUDDY_RELAY_PASSWORD`. Delete the file afterwards; the installer keeps the password in `config.json` (mode 600).

## Tracking estimated spending (optional)

Claude Code's hooks carry no cost data, but its *status line* command receives the session's estimated cost on every
refresh. The installer asks whether to track spending (or pass `--spending yes`). If you say yes:

* `~/.claude/settings.json` gets a `statusLine` entry that runs `statusline.py`. A status line you already had is
  remembered as `statusline.chain` in the config and still runs, with its output shown unchanged.
  `install.py --spending no` or `--uninstall` puts your own one back.
* The bridge keeps the totals in `~/.local/share/claude-buddy/usage.json` and shows them on the web page: this month,
  today, the context window, and a budget bar. The month total is the sum of each session's highest cost seen.
* The numbers are estimates at list prices. Claude Code does not know what your plan or company actually bills. If the
  Claude app shows a total (Settings, Usage), type it into "Set this month's total" on the web page. The first time this
  just lines the two up. Each later time, once the estimate has grown by at least US$5, the buddy works out the ratio
  between how much the app's figure and the estimate grew, and scales new spend by it (limited to 0.5 to 5).
  Spend that never reaches the bridge (other machines, the chat apps) is picked up the same way, as a higher ratio.
