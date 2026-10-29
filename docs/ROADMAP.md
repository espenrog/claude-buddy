# Roadmap

Stages, each ending in a tag. Software-only stages come first because they need no new hardware.

## v0.1: first working version (done)
A Nordic development kit with an SH1106 OLED; nine moods; local and relay modes; installer; SSH-tunnelled relay verified on Windows.

## v0.2: easier install, safer code (in progress)
* Installer explains the choices in plain language, with a diagram, makes "everything on this machine" the obvious
  default, and shows a summary to confirm before changing anything. Split setups (remote Claude, a mirrored buddy) stay available.
* `install.py --doctor`: a self-check that looks at Python, Bluetooth, hooks, bridge, relay and tunnel, and says what is wrong and how to fix it.
* Unit tests for `mood.c` on the host, and tests for the installer and bridge.
* Continuous integration on Linux, macOS and Windows (installer and bridge without Bluetooth).
* A protocol version characteristic, so the bridge can tell if the firmware is too old or too new.
* Fix the relay's password-file permissions.

## v0.3: settings, sessions and a web interface
* A settings characteristic (volume, face profile, brightness, display timeout, quiet hours).
* The bridge serves a local web page (`http://127.0.0.1:8787/`) for status and settings. It works in every browser on every
  OS because only the bridge needs Bluetooth. (A Web Bluetooth page that talks to the buddy directly is possible later, but
  works only in Chromium-based browsers.)
* **Several Claude sessions:** events carry a session slot, the buddy shows how many sessions are running and which need you.

## v0.4: stats and expenses
*Done on the host side: status-line forwarder, totals, budget and calibration on the web page. Firmware 0.4 adds the usage characteristic, dollar-sign eyes and a budget bar. Text stats screens are left for the button work in v0.6 (they need a font and a way to page).*

* A usage feed from Claude Code to the bridge. Hooks carry **no** cost or token data, but the Claude Code *status line*
  command receives a JSON document on every refresh that does. Verified on a live payload: `cost.total_cost_usd` (a per-session
  estimate at list prices), `context_window` (token counts and percent used), `session_id`, `session_name`, model and duration.
  `rate_limits` (5-hour and weekly percentages) were **not** present for an Enterprise account, so those gauges are optional.
  The installer adds a small status-line command (opt-in) that forwards this to the bridge and chains to any status line the user already has.
* The bridge keeps running totals (session, today, this month in US dollars, tokens) in a small state file. The month total is the
  sum of each session's largest cost seen, so it survives missed events and several bridges agree. It resets on the 1st.
* **Calibration:** account-level spend shown in the Claude apps (for example "$14.96 of $500") is not available to Claude Code.
  The estimate tracks it closely, and a "set current total to $X" control in the web page lets you align the two.
* **Budget and dollar eyes:** a monthly budget and warning threshold are settings; above the threshold the eyes turn into dollar signs.
* Stats characteristic and stats screens on the OLED: connection, uptime, tokens, context fullness, cost against budget.

## v0.5: a face with a mouth, and a stats page
* A mouth for every mood, session squares moved to the top left corner.
* A stats page on a button (month, today, budget bar) instead of a permanent bar on the face. Usage format 2 carries the amounts.

## v0.6: power saving and sound
* The display is only updated when the picture changes, and the firmware sleeps until the next blink or animation step
  instead of redrawing 20 times a second. A slower Bluetooth connection (150 to 250 ms) saves radio time.
* Buzzer (passive piezo): sounds for needs-you, finished and limit; reminders; quiet hours (the bridge sets the clock);
  volume and mute from the web page.

## v0.7: battery screen
* A battery screen with icon, percentage and charging state; a blinking low-battery icon on the face and a warning sound;
  a battery characteristic that the fuel gauge fills in (the web page can simulate readings on a build without one);
  the mute button shows "MUTED" / "UNMUTED" on screen.

## v0.8: battery-powered hardware (the Seeed XIAO nRF54LM20A)
* Port to the XIAO nRF54LM20A, with fixes for its SDK board files (power chip pins, Bluetooth controller, RGB LED polarity,
  LDO1 voltage). First flash over its USB-C port with OpenOCD. **Done, verified on boards.**
* Battery gauge and charge status from the nPM1300 (Nordic's fuel gauge library), charge current set for a 200 mAh cell,
  hourly battery log, measurement modes for a Power Profiler Kit; the power was measured (see [HARDWARE.md](HARDWARE.md#power-measured)).
* A unique name per buddy (`Claude Buddy A1B2`) and pairing (LE Secure Connections, a pairing window opened by a button).
* **Firmware update over Bluetooth:** MCUboot with two slots and an SMP server in the application; the bridge's web page sends
  the update. **Done, verified on a board.**
* Accelerometer gestures (shake, pick up, double tap, drop) on the XIAO Sense. Written, not yet run on a Sense.
* A carrier board (Altium, in [hardware/](../hardware/)) and a power switch in series with the cell. **First boards built and working.**

## v0.9: polish
* A model of the real cell for the fuel gauge (nPM PowerUP), so the percentage is accurate.
* Tune the gesture thresholds on a Sense, and measure the accelerometer's current.
* More stats pages (context, sessions); long presses.

## v1.0: ready for other people
* Enclosure (3D-printable), bill of materials, photographed build guide.
* Release checklist, including a license check for any prebuilt firmware.

## Open questions
* **Transcript files are not a source:** `~/.claude/projects/**/*.jsonl` is an internal format that changes between versions.
* **Account-level spend** (Console usage pages, OpenTelemetry export) exists but needs credentials or a collector; out of scope for a desk gadget.
* **What "expenses" means:** Claude Code reports an *estimate* at list prices; a Pro, Max or Enterprise seat is not billed per token.
  The screen says "est." and the docs explain it.
