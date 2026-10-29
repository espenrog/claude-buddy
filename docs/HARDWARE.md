# Hardware

The buddy runs on a **Seeed Studio XIAO nRF54LM20A**: a 21 x 17.8 mm module with the nRF54LM20A, an nPM1300 power chip
(charger and fuel gauge) and a USB-C port that doubles as the programming probe. Everything else is a display, a buzzer, two
buttons and a small Li-Po cell. The step-by-step build is in [BRINGUP.md](BRINGUP.md); this page is the reference.

Status: runs on real boards (first one 2026-10-22). Flashing over USB-C, Bluetooth with the unique name, pairing, the fuel
gauge, the OLED, the buzzer, the buttons and the pairing holds, updates over Bluetooth, the carrier board and charging at 100 mA
all work. Checked against Seeed's V1.0 schematic and KiCad files (published under CC BY-SA 4.0) and NCS v3.5.0-preview3.

## Parts

| Part | Notes |
|---|---|
| Seeed **XIAO nRF54LM20A** | Plain or Sense. Only the Sense has the accelerometer (gestures, see below); everything else is the same. |
| The XIAO's antenna | **Fit it.** Without it the range is a few centimetres and the bridge cannot connect, which looks like a firmware fault. |
| **1.3" 128x64 OLED, SH1106, I2C** | White on black, usually 4 pins (GND, VCC, SCL, SDA), I2C address 0x3C. The pin order differs between vendors: check yours. |
| Passive piezo buzzer (optional) | Not an active buzzer (those only beep at one pitch). Without one the firmware runs and makes no sound. |
| 2 push buttons | Mute (also pairing) and page, each to GND. No resistors needed. |
| Li-Po cell, 150-200 mAh, with a protection circuit | The board has no thermistor input, so the pack's protection circuit is the only guard. |
| Power switch (optional) | In series with the cell's + wire, see [Power switch](#power-switch). |
| Carrier board (optional) | Holds the XIAO, an OLED connector, the buzzer, two buttons, the power switch and a battery connector. Altium design files: `hardware/Altium_claude_buddy_v0.3_2026-10-29.zip`. |

Other SSD1306-family displays should work with a devicetree change in the overlay: Zephyr's SSD1306 driver also drives
SH1106 parts.

## Pins

Set in `firmware/boards/xiao_nrf54lm20a_nrf54lm20a_cpuapp.overlay`:

| Header pin | GPIO | Use |
|---|---|---|
| D4 / D5 | P1.03 / P1.07 | OLED SDA / SCL (the board's default XIAO I2C, i2c22) |
| D3 | P1.29 | buzzer, PWM21, other leg to GND |
| D2 | P1.30 | mute button to GND (internal pull-up); also the pairing button |
| D1 | P1.31 | page button to GND (internal pull-up) |
| (user button) | P0.09 | the board's own small button: poke |
| D0 | P1.00 | free |
| 3V3, GND | | OLED supply and ground |

D0 to D3 are the header pins with no default peripheral, so the board's uart21 (D6/D7), spi23 (D8 to D10) and i2c22 stay usable.

Buttons in the firmware (`main.c`):

* **Poke** (the board's button): five presses within four seconds make the buddy angry. Any press wakes a dark display.
* **Mute** (D2): a short press toggles mute (acted on at release) and shows `MUTED` or `UNMUTED`. Holding it 3 s opens the
  pairing window (`PAIRING`); 8 s forgets every pairing.
* **Page** (D1): steps through the stats page, the battery page and back to the face.

There is no eye-style button: set the eye style from the web page.

## What the schematic says

* **USB-C goes only to the SAMD11** (a CMSIS-DAP probe and a UART bridge to the nRF's UART20). The nRF has no USB
  connection: flashing and the serial log both go through the USB-C port.
* The SAMD11 is powered through a load switch enabled by VBUS, so it should not drain the battery (not measured on its own).
* **Power:** the nRF runs from the nPM1300's BUCK2 (3.3 V, 200 mA). The header's 3V3 pin is a separate always-on buck
  (TPS628438, 600 mA, output only). Charging, ship mode (SHPHLD) and the fuel gauge are the nPM1300's.
* **No cell thermistor input:** the nPM1300's NTC pin has a fixed 10 k resistor to ground, so a pack's own NTC wire cannot be
  used. The firmware reports no cell temperature (`CONFIG_BUDDY_CELL_THERMISTOR=n`; the gauge assumes 25 C). Rely on the pack's
  protection circuit, and set the charge current for your cell (see [Battery](#battery)).
* **Battery connection:** VBAT (TP9), a second test point and SHPHLD are pads on the **bottom** side. On the carrier board they
  meet the carrier's BAT holes; wired by hand, solder wires to them before mounting the board.
* The nPM1300 is on I2C **P1.17 (SCL) / P1.18 (SDA)**, with 4.7 k pull-ups on the board.

## Problems in the SDK's board files (worked around in the overlay)

1. The board file puts the nPM1300's I2C on P1.15/P1.16: wrong (see above), so the gauge would never find the chip.
2. The board selects Zephyr's own Bluetooth controller, which does not build in the SDK; the overlay selects Nordic's and
   enables the radio and clock nodes it needs.
3. MCUboot cannot link with the board's power chip node: `firmware/boards/xiao_nrf54lm20a-mcuboot.overlay` removes it (and an
   unconnected enable regulator on P1.12) for the MCUboot image only.
4. The RGB LED on P1.22/23/24 lights when its pin is low, but the board file declares it active high. The LED driver (which the
   fuel gauge pulls in) then drives it on as "off": about 1.5 mA wasted. The overlay declares it active low.
5. The nPM1300's LED outputs light the board's charge LEDs at their reset default; the overlay puts them in host mode (off).
6. The accelerometer and microphone run from the nPM1300's LDO1, which the board file sets to **1.8 V**, while the nRF's pins
   are at 3.3 V and the sensor's I2C pull-ups go to its own supply. Going by the schematic the nRF cannot read that bus. The
   overlay sets LDO1 to 3.3 V (the sensor takes up to 3.6 V) and turns the regulator driver on. Not yet confirmed on a Sense.

## Accelerometer (shake, pick up, double tap, drop)

**Needs the XIAO nRF54LM20A Sense.** The plain board has no IMU (and no microphone), although the SDK's board file and Seeed's
schematic describe the Sense. On a plain board `i2c scan` finds nothing, the firmware logs `No accelerometer (err -5, id 0x00)`
and carries on without gestures.

The Sense's LSM6DS3TR-C (i2c30 on P0.07/P0.08, INT1 on P0.06, address 0x6a) is set up by `firmware/src/motion.c`, not by
Zephyr's driver (`CONFIG_LSM6DSL=n`: it has no tap or free-fall interrupts). The sensor watches in hardware and raises INT1;
the processor only reads samples for 1.5 s after a movement, to tell a shake from a pick-up (`firmware/src/shake.c`).

| Gesture | What the sensor sees | What the buddy does |
|---|---|---|
| Double tap on the case | double tap (750 mg, up to ~0.5 s apart) | a poke (5 in 4 s make it angry) |
| Picked up / moved | movement over 125 mg | lights a dark display for 10 s |
| Shaken | 4 swings of more than 1 g on one axis, alternating, within 1.5 s | **dizzy** for 4 s (spiral eyes) |
| Dropped | under 312 mg for ~80 ms (free fall) | **scared** for 2.5 s, plays a falling whistle |

Alerts and the limit screen still win over dizzy and scared. A held measurement mode ignores all of it. The thresholds are
first guesses, not yet tuned on a board. Tap detection needs the sensor at 416 Hz: expect roughly 0.1 to 0.2 mA extra
(an estimate, not measured).

## Battery

The firmware charges at **100 mA to 4.15 V** (`&pmic_charger` in the overlay): 0.5C for a 200 mAh cell such as an LP402030.
For a smaller cell lower the current there; the board file's own default is 150 mA. The gauge uses the SDK's example cell
model unless you build with a model of your own cell (see [FIRMWARE.md](FIRMWARE.md#battery-model)), so take the percentage as
a rough figure.

Before a cell is fitted, the log and the web page show about 4.17 V and 99 %: that is the charger's output, not a cell.

## Power measured

PPK2 as source meter on VBAT, USB unplugged, 3.9 V, bridge connected:

| State | Current |
|---|---|
| Display off (idle) | 0.187 mA |
| Face at rest | 3.54 mA (3.75 mA at 3.6 V, 3.46 mA at 4.1 V) |
| Face animated | 3.35 mA |
| Stats page | 5.65 mA |
| Battery page | 7.31 mA |
| Advertising only (no bridge), face on | 2.92 mA |

With 3 h of face a day and 21 h idle that is about 14.6 mAh a day, so about 122 mAh for a week with 20 % margin (about 180 mAh
at 5 h a day). A 150 mAh cell covers the 3 h case; 200 mAh leaves room for more use and the cold. Idle was measured only at
3.9 V. How to repeat the measurements: [POWER-MEASUREMENT.md](POWER-MEASUREMENT.md).

## Power switch

An optional slide switch in series with the cell's + wire turns the buddy fully off (no standby drain). With the switch off,
USB-C still powers the buddy (and charges nothing).

The switch used so far: EAO 09.03290.01, series 1K2 (DigiKey 1948-1087-ND,
[datasheet](https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/496/series1k2_201004.pdf)), a one-pole changeover
slide switch with 3 pins in a row at 2.54 mm pitch:

| Pin | Role |
|---|---|
| 1 (middle) | common |
| 2 (outer) | connected to 1 in slider position A |
| 3 (outer) | connected to 1 in slider position B |

Wiring: cell + to pin 1, one outer pin to the XIAO's **VBAT**, the other outer pin **left unconnected**. Which outer pin you
use only decides which slider direction is "on". Check it with a multimeter before soldering, and cover the pins that stay
live while the switch is off. Rated 500 mA, far above the 100 mA charge current. The datasheet asks for hand soldering at
350 to 370 C for no more than 5 s per pin, and no ultrasonic, immersion or water-jet cleaning.

## Notes

* The OLED module must accept 3.3 V logic. Many modules pull SDA/SCL up to their own supply: power it from the XIAO's 3V3
  pin, not from 5 V.
* Other pins: change them in the overlay and in this page together.
