# Troubleshooting

Every entry here is something that actually went wrong while building the project.

## Nothing happens at all

**Start with `python3 install.py --doctor`** on each machine: it checks the whole chain and suggests a fix for each problem.
If you want to investigate by hand, work from the end of the chain backwards.

1. **Does the buddy react to a manual event?**
   `python3 ~/.local/share/claude-buddy/emit.py prompt_submitted` (eyes should sweep).
   If yes, the hooks are the problem (step 4). If not, continue.
2. **Is the bridge running?** `python3 install.py --status`, then look at its log (see [SETUP.md](SETUP.md)).
   It should say `BLE connected`. If it keeps scanning, the buddy is out of range, unpowered, or already
   connected to another device (the buddy accepts one connection; disconnect your phone's BLE app).
3. **Does the bridge receive the event?** Look for `event prompt_submitted -> 0x03` in its log. If not, see
   *MQTT connection resets* below (relay setups) or check the HTTP port (local setups).
4. **Are the hooks installed?** `python3 install.py --status` should list 12 hooks. Claude Code only reads hooks when
   a session starts, so restart running sessions.

## The display

| Symptom | Cause and fix |
|---|---|
| Serial log: `Failed to suspend: -5`, `Failed to initialize device`, `Display device not ready; running headless` | The OLED doesn't answer on I2C (`-5` = no acknowledge). Check wiring (SDA/SCL swapped?), power, and the address: many modules are 0x3C, some 0x3D (change `reg` and the node name in the overlay). The firmware keeps running without a display, so Bluetooth still works. |
| Random "snow" on the screen | The display is powered but never initialised: the I2C commands don't arrive. Usually wiring, or the module is powered from the wrong pin: use the XIAO's 3V3 (see [HARDWARE.md](HARDWARE.md)). |
| `display_write failed (-28)` and `Need to use the internal driver buffer but its size is insufficient (1 + 128 > 16)` | Add `zephyr,concat-buf-size = <256>;` and `zephyr,flash-buf-max-size = <256>;` to the I2C node in the overlay (already in the provided overlay). |
| Picture shifted by a few pixels, wrapped or mirrored | SH1106 controllers have 132 columns, not 128. Adjust `segment-offset` (2 is typical), `segment-remap`, `com-invdir`. |
| Colours inverted | Toggle `inversion-on` in the overlay. |

Finding an unknown I2C address: build with the diagnostic shell and scan the bus.

```bash
west build ... -- -DEXTRA_CONF_FILE=diag-i2c-shell.conf
# then, in the serial terminal at the "uart:~$" prompt:
i2c scan i2c@c8000
```

## MQTT connection resets

Symptom: the bridge log shows `Connection reset` / `WinError 10054` right after "connecting", and the broker
log shows connections that open and close without a login. A plain TCP connect test (`Test-NetConnection`, `nc -z`)
succeeds, which is misleading: something on the path (often a corporate VPN or firewall) resets the connection
as soon as it sees MQTT data.

Fix: use the SSH tunnel option. Re-run `install.py` on the bridge machine and answer yes to
*Reach the relay through an SSH tunnel?* (needs key-based SSH to the relay machine). The tunnel carries the
MQTT traffic inside SSH, which these middleboxes leave alone.

## Testing from a phone BLE app fails

Symptoms with *nRF Connect for Mobile* (Android): the write seems to be accepted but nothing happens, and the app
log shows `GATT PREPARE Q FULL (0x9)`; before that, the firmware log showed `Unhandled ATT code 0x16`.

Cause: the app sends the byte as a *prepared write* (queued, to be committed by a later Execute) and never
commits it. The firmware is behaving correctly: it accepts prepared writes but only acts on committed ones.
The bridge uses ordinary write requests and is unaffected. Test with `emit.py` instead.

## The bridge won't find the buddy

* **Linux:** needs BlueZ running (`systemctl status bluetooth`) and an adapter that supports BLE.
* **macOS:** allow Bluetooth for Python (System Settings, Privacy and Security, Bluetooth) when prompted.
* **Windows:** Bluetooth must be on. The bridge must run in your normal desktop session (it does, via the logon task);
  running it from an SSH session usually fails because the Windows Bluetooth API is unavailable there.
* The bridge matches the **service UUID**, so a renamed device is still found.

## "Not paired yet" on the web page

Firmware 0.8.10 and newer only talks to a paired bridge (see PROTOCOL.md, Pairing). A brand new or just-erased buddy opens its
pairing window by itself for two minutes after power-up; otherwise hold the mute button for 3 seconds until the display
says `PAIRING`. The bridge asks the operating system to pair; on Windows a "connect" prompt may appear. If pairing keeps
failing after the buddy was erased or its pairings were forgotten, remove "Claude Buddy" from the computer's Bluetooth
devices first (stale keys), then open the pairing window again.

## Installer problems

| Symptom | Fix |
|---|---|
| `Installing the Python dependencies failed` | pip couldn't download `bleak` / `paho-mqtt`. Check your internet access, and that this Python was built with SSL support (`python3 -c "import ssl"`). Use a different Python (python.org build, `pyenv` with OpenSSL) and re-run. |
| `Docker was not found` | The relay needs Docker. Install it, or run any MQTT broker yourself and choose "use an existing relay". |
| Linux: bridge doesn't start after reboot | `loginctl enable-linger $USER` (the installer tries this) lets user services run without a login session. |
| Windows: two `pythonw.exe` processes are running | Normal. A virtual environment's `pythonw.exe` is a launcher that starts the real interpreter as a child. Check `ParentProcessId`: if one is the other's parent it is a single bridge. Two unrelated ones would fight over the buddy's single BLE connection; stop the older one. |
| Windows: task doesn't start | Open *Task Scheduler*, find `ClaudeBuddyBridge`, and check *Last Run Result*; the log is `%USERPROFILE%\.local\share\claude-buddy\bridge.log`. |

## The bridge cannot connect, or only from very close

Fit the XIAO's antenna. Without it the range is a few centimetres, which looks like a firmware fault. Also check that no
other computer's bridge holds the connection: the buddy accepts one at a time.

## Flashing

* A new XIAO is locked: the first OpenOCD run erases and unlocks it, then stops. Run the same command again
  (see [FIRMWARE.md](FIRMWARE.md#first-flash-over-usb-c)).
* `Error: unable to open CMSIS-DAP device` on Linux: your user needs access to USB ID 2886:0068 (a udev rule, or try once with
  `sudo` to confirm that is the problem).
* OpenOCD prints no "downloaded" or "verified" line for this chip: check the serial log instead.
* An update over Bluetooth is refused: the image was signed with a different key than the one in the buddy's first flash.
