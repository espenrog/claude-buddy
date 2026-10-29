# Firmware

Zephyr application for the nRF Connect SDK, for the Seeed XIAO nRF54LM20A (board target
`xiao_nrf54lm20a/nrf54lm20a/cpuapp`). Built with MCUboot (swap using move), signed with your own key, and updatable over
Bluetooth from the bridge's web page. The first flash goes over the XIAO's USB-C port; no J-Link is needed.

## Build

The board is in NCS **v3.5.0-preview3** and newer. Install the SDK with
[nRF Util](https://docs.nordicsemi.com/bundle/nrfutil/page/guides/installing.html) (`nrfutil install sdk-manager`) or the
nRF Connect for VS Code extension. No toolchain is registered for v3.5.0-preview3 (`--ncs-version` fails), so the scripts use
the v3.4.1 toolchain bundle `fbf7391cab` with the preview3 tree; set `BUDDY_TOOLCHAIN=<id>` (from `nrfutil sdk-manager list`)
to use another one.

Once, create your signing key and **back it up** (without it you can never update a buddy flashed with it again; never put it
in git):

    tools/new-signing-key.sh ~/ncs/v3.5.0-preview3

Then build:

    tools/build-xiao.sh ~/ncs/v3.5.0-preview3 <build dir> ~/.config/claude-buddy/signing/claude-buddy-ed25519.pem

It writes:

* `<build dir>/claude-buddy-xiao-all.hex`: MCUboot and the application, for the first flash.
* `<build dir>/dfu_application.zip`: the application alone, for updates over Bluetooth.

The image layout: 62 kB bootloader, two 968 kB image slots, a storage partition for the settings and pairings (updates do not
touch it). The application is about 280 kB. The public key is built into MCUboot (`firmware/sysbuild.conf`), not provisioned
into the chip's key store: the USB-C probe cannot do that.

## First flash (over USB-C)

The USB-C port is a CMSIS-DAP probe (the SAMD11). Stock OpenOCD 0.12.0 (`apt install openocd`) is enough; Seeed's board
script (`<ncs>/zephyr/boards/seeed/xiao_nrf54lm20a/support/openocd.cfg`) defines the load command:

    openocd -f openocd.cfg -c "init; reset halt; nrf54lm20a-load <hex>; verify_image <hex>; reset run; exit"

* A new board is AP-locked: the first run erases and unlocks it by itself (the factory firmware is gone) and then stops. Run
  the same command again.
* OpenOCD prints no "downloaded" or "verified" lines for this chip; judge by the serial log.
* On Linux your user may need a udev rule for the probe (USB ID 2886:0068).
* The Zephyr SDK host tools carry no OpenOCD, and neither do the NCS toolchains on Linux.

## Updates over Bluetooth

On the bridge's web page, **Firmware update**: choose the `dfu_application.zip` and send it. The bridge lets go of the link,
connects again over the paired, encrypted link, uploads into the second slot, marks it for a test run and resets the buddy.
MCUboot checks the signature and swaps the images; the new firmware confirms itself once Bluetooth is up (`smp_update.c`). If
it does not get that far, the next reset goes back to the old one. Only a paired bridge can send an update, and MCUboot runs
only images signed with your key. Protocol details: [PROTOCOL.md](PROTOCOL.md#firmware-updates-capability-bit-5).

## What you should see

* Serial log (the SAMD11's UART bridge: `/dev/ttyACM*` on Linux, a COM port on Windows, 115200 baud): `Name: Claude Buddy
  A1B2`, `Pairing window open for 120 s` (while nothing is paired), `Advertising`, then `Connected`, and `Event 0x03` and
  `Mood 1 -> 3` for each event. `Failed to initialize chip` or `No accelerometer` early on is harmless on a plain XIAO.
* The OLED shows the face, with X eyes while no bridge is connected.
* The buttons as listed in [HARDWARE.md](HARDWARE.md#pins).

## Code layout

```
firmware/
├── CMakeLists.txt
├── Kconfig                       the buddy's options (fuel gauge, pairing, updates, battery simulation)
├── prj.conf                      Bluetooth, display, I2C, settings, logging
├── sysbuild.conf                 MCUboot, swap using move, ed25519 signature
├── diag-i2c-shell.conf           optional: adds an `i2c scan` shell (see TROUBLESHOOTING)
├── boards/
│   ├── xiao_nrf54lm20a_nrf54lm20a_cpuapp.overlay   pins, display, buzzer, buttons, power chip fixes
│   ├── xiao_nrf54lm20a_nrf54lm20a_cpuapp.conf      board options (no cell thermistor, accelerometer)
│   └── xiao_nrf54lm20a-mcuboot.overlay             removes the power chip from the MCUboot image
└── src/
    ├── main.c          init, buttons
    ├── ble_svc.c       advertising, pairing, GATT service
    ├── ui.c            owns the display; drains the event queue; renders when the picture changes
    ├── mood.c          state machine for up to 8 sessions (pure logic, no Zephyr dependencies)
    ├── face.c          draws each mood into a 128x64 1-bit page-tiled framebuffer
    ├── buddy_cfg.c     settings, their wire format and validation (pure logic)
    ├── cfg_store.c     keeps the settings in RAM and saves them to non-volatile memory
    ├── battery.c       battery state, wire format and warning levels (pure logic)
    ├── battery_gauge.c reads the nPM1300 and runs Nordic's fuel gauge library
    ├── battery_log*.c  the hourly battery log and its storage
    ├── buzzer.c, sound.c  PWM buzzer and the sound patterns
    ├── buddy_clock.c   local time from the bridge, for quiet hours
    ├── measure_mode.c  held states for power measurements
    ├── motion.c, shake.c  accelerometer setup and gesture detection (XIAO Sense)
    └── smp_update.c    confirms a new image after an update
```

Design rules followed: hardware in devicetree, features in Kconfig, `main()` is thin, nothing blocks in Bluetooth callbacks
(events go through a message queue to a work item). The pure-logic files are also compiled and tested on the host
(`python3 -m pytest -q`).

## Changing things

* **Faces:** `face.c`. Each mood is a short drawing function using rectangles and lines.
* **Timings** (bored after 5 minutes, asleep after 30, hold times): constants at the top of `mood.c`.
* **A different display or pins:** edit the overlay. Keep `zephyr,concat-buf-size = <256>` on the I2C node, because one display
  row is sent as a single 129-byte transfer.
* **The buzzer pin:** change the `&pwm21` pinctrl block and the `zephyr,user` node in the overlay (PWM pins must be on a port the
  instance supports: see the board's pinctrl file).
* **New event:** add a code to `enum claude_event` in `mood.h`, handle it in `mood_on_event()`, and add the same name and code
  to `EVENT_CODES` in `bridge/claude_buddy_bridge.py` and a hook entry in `install.py`.

## Buzzer

A **passive piezo** buzzer between **D3** and **GND**, driven straight from the pin by PWM21. A magnetic buzzer needs a
transistor and a diode. The patterns are listed in [PROTOCOL.md](PROTOCOL.md#sounds). Volume, mute and quiet hours are
settings on the web page.

## Power use

The firmware avoids work it does not need:

* It sleeps until the picture can next change (the next blink, the next animation step, a timeout) and does not redraw the
  idle face 20 times a second. A chill face wakes about once a second.
* It sends a frame to the display only when it differs from the one already shown.
* It asks the central for a slow connection (150 to 250 ms, may skip one event). Events can arrive up to half a second late.
* The display can dim while the buddy sleeps, and switch off.

Measured currents: [HARDWARE.md](HARDWARE.md#power-measured).

## Battery model

The fuel gauge needs a model of the cell. By default the build uses the example model from the SDK's
`samples/pmic/native/npm13xx_fuel_gauge` sample, which gives only a rough percentage. For a real cell, make a model with the
nPM PowerUP app (or save one of its bundled profiles) and build with `BUDDY_BATTERY_MODEL_DIR=<directory with
battery_model.inc>` set in the environment. The model is not part of this repository: the SDK's files have their own license.

## Battery simulation

`CONFIG_BUDDY_BATTERY_SIMULATION` (off by default, since the XIAO has a real gauge) lets the battery characteristic accept
test readings, so the battery screen and the low-battery warning can be tried from the web page on a build without a gauge.
The bridge's `--simulate` mode has the same test section without any hardware.
