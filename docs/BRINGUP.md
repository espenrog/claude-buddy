# First bring-up of a new buddy (XIAO nRF54LM20A)

From a bare Seeed XIAO nRF54LM20A to a buddy on your desk that follows Claude Code, either on the buddy carrier board
(design files: the Altium zip in [hardware/](../hardware/)) or wired by hand. Go through the steps in order and
check the **You should see** line before moving on; if it does not match, the step after it will not work either.
Background and details: [HARDWARE.md](HARDWARE.md) (board, pins, power), [FIRMWARE.md](FIRMWARE.md) (build, flash,
updates), [SETUP.md](SETUP.md) (installer),
[TROUBLESHOOTING.md](TROUBLESHOOTING.md).

Time: about an hour, most of it soldering.

## 0. What you need

| Item | Notes |
|---|---|
| Seeed XIAO nRF54LM20A | Plain or Sense. Only the Sense has the accelerometer (gestures); everything else is the same |
| The XIAO's antenna | **Fit it.** Without it the range is a few centimetres and nothing connects |
| Buddy carrier board (optional) | Holds the XIAO, OLED connector, buzzer, two buttons, power switch and a battery connector. Without it, wire the parts by hand (step 4b) |
| 1.3" SH1106 OLED, 128x64, I2C | Check your module's pin order (VCC/GND/SCL/SDA differs between vendors) |
| Passive piezo buzzer | Not an active buzzer (those only beep at one pitch) |
| 2 push buttons | Mute and page, each to GND |
| LiPo cell, 150-200 mAh, with protection circuit | Optional for the first run (USB-C powers it). The board has no NTC input, so the protection circuit is the only guard |
| USB-C data cable | Flashing, power and the serial log all go through it |
| A computer with Bluetooth LE | Runs the bridge; Claude Code can be here or elsewhere ([SETUP.md](SETUP.md)) |

Software: `openocd` 0.12.0 or newer (`sudo apt install openocd`, or your OS's package) and Python 3.9+. To build the
firmware yourself you also need nRF Connect SDK v3.5.0-preview3 or newer (step 1b).

## 1. Get the firmware

You need two files:

* `claude-buddy-xiao-all.hex`: MCUboot + application, for this first flash over USB-C.
* `dfu_application.zip`: the same firmware for later updates over Bluetooth.

**1a. You already have them** (built by whoever makes your buddies, signed with their key): skip to step 2. Updates later
must be signed with that same key.

**1b. Build them yourself.** Once, create your signing key and **back it up** (without it you can never update this buddy
over Bluetooth again; never put it in git):

    tools/new-signing-key.sh ~/ncs/v3.5.0-preview3

Then build (writes both files into the build directory):

    tools/build-xiao.sh ~/ncs/v3.5.0-preview3 ~/buddy-build ~/.config/claude-buddy/signing/claude-buddy-ed25519.pem

**You should see:** `~/buddy-build/claude-buddy-xiao-all.hex` and `~/buddy-build/dfu_application.zip`.

Both scripts run inside the nRF Connect SDK toolchain bundle `fbf7391cab`. If `nrfutil sdk-manager list` shows another
one, run them with `BUDDY_TOOLCHAIN=<its id>` in front.

## 2. Flash it (over USB-C, no J-Link needed)

**On the carrier board:** solder the XIAO and do the checks in step 4a **before** you plug USB-C in for the first time.

Fit the antenna now. Plug the XIAO into the computer that has OpenOCD. Copy Seeed's board script next to the hex
(`<ncs>/zephyr/boards/seeed/xiao_nrf54lm20a/support/openocd.cfg`), then:

    openocd -f openocd.cfg -c "init; reset halt; nrf54lm20a-load claude-buddy-xiao-all.hex; verify_image claude-buddy-xiao-all.hex; reset run; exit"

* A new board is **locked** from the factory. The first run erases and unlocks it, then stops. **Run the same command a
  second time**: that one programs it. This is normal.
* `Error: unable to open CMSIS-DAP device` on Linux: your user needs access to USB ID 2886:0068 (a udev rule, or try once
  with `sudo` to confirm that is the problem).
* OpenOCD prints no "downloaded" or "verified" line for this chip. Judge by the next step.

## 3. Check the serial log

Open the XIAO's serial port at 115200 baud (`/dev/ttyACM0` on Linux, a COM port on Windows), for example
`python3 -m serial.tools.miniterm /dev/ttyACM0 115200`, and press the reset button on the XIAO.

**You should see**, among other lines:

    Name: Claude Buddy 87E5
    Pairing window open for 120 s
    Advertising

Write down the four characters after "Claude Buddy": that is this buddy's name. `Failed to initialize chip` or
`No accelerometer` early on is harmless (no accelerometer, or a plain XIAO).

Nothing at all on the port? The flash did not take: go back to step 2 and run the command again.

## 4. Connect the display, buzzer and buttons

### 4a. On the carrier board

The XIAO sits on the carrier's back side; USB-C and the antenna connector stay reachable. Solder it in three parts:

1. The castellated edge pads (D0-D10, 5V, GND, 3V3 and the rest of the header).
2. The battery pair: the carrier's two-pin BAT holes sit over the XIAO's bottom test points. Pin 1 = VBAT, pin 2 = GND.
3. Optional, for a J-Link: the 2x4 SWD holes, also over the bottom test points.

**Before the first power-up** (multimeter, nothing plugged in, no cell):

* No short to GND on 5V (VBUS), the OLED supply (connector pin 2), VBAT and the battery connector's +. Solder bridges
  under the edge pads are the usual cause.
* GND of the XIAO beeps through to the carrier's GND.
* BAT pin 1 beeps to the XIAO's VBAT, pin 2 to GND.
* Power switch on: the battery connector's + beeps to VBAT. Off: open.

Now flash it (steps 2 and 3), then fit the OLED to its connector. Buzzer and buttons are already on the board: button A
(D2) is mute and pairing, button B (D1) is page. Then do the checks at the end of 4b.

### 4b. Wired by hand

Unplug USB-C first. All parts go to the XIAO's header pins (pin table in [HARDWARE.md](HARDWARE.md#pins)):

| Part | XIAO pin |
|---|---|
| OLED VCC / GND | 3V3 / GND |
| OLED SDA / SCL | D4 / D5 |
| Buzzer | D3 and GND |
| Mute button (also the pairing button) | D2 to GND |
| Page button | D1 to GND |

The buttons need no resistors (internal pull-ups). D0 stays free. The board's own small button is "poke".

Plug USB-C back in. **You should see:** the face on the OLED, with X eyes (no bridge connected yet). Press page: the
stats page appears; again: the battery page; again: back to the face. Press mute: the display shows `MUTED`, then
`UNMUTED` on the next press.

## 5. Install the bridge

On the computer that sits next to the buddy, in a copy of this repository:

    python3 install.py          (Windows: py install.py)

Pick option 1 if Claude Code runs on the same computer, otherwise see [SETUP.md](SETUP.md) for options 2-4. Then:

    python3 install.py --doctor

**You should see:** everything green, apart from "buddy not connected" (it is not paired yet).

## 6. Pair

The buddy only talks to a paired bridge. Open the pairing window: power-cycle the buddy (a new buddy opens it for
2 minutes after power-up), or **hold mute (D2) for 3 seconds** until the display says `PAIRING`. Within those 2 minutes
the bridge finds it and asks the OS to pair; on Windows, **accept the "connect" pop-up**.

**You should see:** `PAIRED` on the display, the face with normal eyes, and on http://127.0.0.1:8787/ the buddy's
name, firmware version and a battery figure.

If it says "Not paired yet" and the window has closed, open it again (hold mute 3 s). If pairing keeps failing, remove
"Claude Buddy" from the computer's Bluetooth devices and try again.

**More than one buddy** (a new one next to an old one): one bridge serves one buddy. Without a name it takes the nearest
one when it connects, and stays with it. To choose, add the four-character name to the `"bridge"` section of
`~/.config/claude-buddy/config.json` (Windows: `%USERPROFILE%\.config\claude-buddy\config.json`):

    "bridge": { ..., "ble_name": "87E5" }

Restart the bridge (Linux: `systemctl --user restart claude-buddy-bridge`; Windows PowerShell:
`Stop-ScheduledTask -TaskName ClaudeBuddyBridge; Start-ScheduledTask -TaskName ClaudeBuddyBridge`), then open the
pairing window on the new buddy. The old one does not need to be switched off; it just stays unconnected. To serve both,
run a bridge for each on its own computer.

## 7. Check it follows Claude Code

On the web page, press a few of the test event buttons: the face changes and the buzzer beeps (unless muted or in quiet
hours). Then start a Claude Code session and give it a task.

**You should see:** the face changes while Claude works, and an alert (plus a beep) when it waits for you.

## 8. Battery (optional)

Unplug USB-C. Check the cell's polarity twice before connecting: many cheap packs have the red and black wires the other
way round in their plug.

* **Carrier board:** power switch off, plug the cell into the battery connector, then switch on.
* **Wired by hand:** solder the cell to the **VBAT and GND pads on the bottom** of the XIAO (red to VBAT).

Before a cell is fitted, the log and the web page show about 4.17 V and 99 %: that is the charger's output, not a cell.

**You should see:**

* With USB-C unplugged: the buddy keeps running, and the battery page and the web page show a percentage and a voltage
  (3.5-4.2 V) and a negative current.
* With USB-C plugged in: "charging" and a positive current of about 100 mA (less once the cell is nearly full).
* On the carrier: with the switch off, USB-C still powers the buddy.

The firmware charges at 100 mA to 4.15 V (0.5C for a 200 mAh cell; set in the XIAO overlay). For a smaller cell lower
it there. No cell temperature is shown: the board has no thermistor input. The percentage uses the SDK's example cell
model, so take it as a rough figure.

## 9. Update over Bluetooth (from now on)

No more USB-C needed. On the web page, Firmware update: choose the new `dfu_application.zip` (signed with the same key as
step 1) and send it. The buddy restarts and swaps the images (give it a minute).

**You should see:** the new firmware version on the web page, and `New firmware confirmed` in the serial log if you have it
open. If the new firmware fails to start, the buddy goes back to the old one by itself on the next reset.

## Done

Label the buddy with its four-character name. Keep the signing key backed up.

| Problem | Most likely cause |
|---|---|
| Bridge cannot find or connect to the buddy | Antenna missing; another computer's bridge already holds the connection (one at a time) |
| Display stays black, log looks fine | OLED SDA/SCL swapped, or the module's VCC/GND pins are in another order |
| Buddy rejects an update | Image signed with a different key than the one in its first flash |
| No gestures | Plain XIAO: gestures need the Sense board |
| Bridge stays with the old buddy | Set `bridge.ble_name` and restart the bridge (step 6) |
| Carrier: nothing on the serial port, or the XIAO gets warm | A solder bridge under the edge pads: redo the checks in step 4a |
| Runs on USB-C only, not on the cell | Power switch off, cell plug reversed, or the BAT pair not soldered |
