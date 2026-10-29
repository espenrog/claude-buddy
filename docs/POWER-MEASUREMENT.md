# Power measurement with a Power Profiler Kit

Goal: measure the buddy's current in each state and size the battery for one week of use (assumed use: face on about 3 h per
day, display blanked the rest of the day). Results for the XIAO nRF54LM20A: [HARDWARE.md](HARDWARE.md#power-measured).

## Without a PPK: the battery log

Firmware 0.8.7 and newer records the charge, voltage, average battery current and display-lit time once an hour (docs/PROTOCOL.md),
and the bridge writes them to `~/.local/share/claude-buddy/battery-log.csv`. Run normally for a few days on battery, then add up the
`used_mah` column per day (it is empty for hours on USB power). The current is the power chip's own reading and is coarse
below about a milliampere, so also compare the percent column: 1 % of the cell is its capacity / 100. The log is a cross-check
and a way to see real use; the PPK measurement below gives the precise figures.

## Prepare the firmware

1. Use the current release build and keep the bridge connected: the real idle current includes the BLE link
   (150 to 250 ms interval).
2. The sleeping mood only dims the display to `sleep_brightness` (default 8). It blanks the display only when that
   setting is 0. Set sleep brightness to 0 in the settings on the web page, so the idle state really blanks the
   display, and check that the status shows the display as off.
3. The buddy gets bored 5 minutes after the last event and falls asleep after 30 (`mood.c`), so the face stays lit for
   about half an hour after you stop working. Count that in the hours per day.

## Wiring (PPK2 in source meter mode)

1. Open the power switch and take the battery out of the circuit. Do not power the board from USB-C: the charger then
   runs from VBUS and distorts the numbers.
2. PPK2 VOUT to the XIAO's VBAT pad (bottom side, or the carrier's battery connector), PPK2 GND to GND.
3. Set the source voltage to 3.9 V (typical mid-charge). Repeat the idle measurement at 3.6 V and 4.1 V.
4. Check the PPK2 current limit against the peaks (OLED start-up, buzzer), which can be tens of mA.

## Measurement modes

Firmware 0.8.8 and newer holds the buddy in one state for you: open the bridge's web page and use the **Measure power**
card (the card only shows with a connected buddy that has the feature). Pick a mode, read the PPK, pick the next. The modes:
display off, face at rest, face animated, stats page, battery page, everything lit (the upper bound), one beep, back to normal.
A held mode ignores Claude, makes no sound of its own, and ends by itself after 30 minutes or on a restart. The battery log marks
the hours it covered. The advertising-only case (no bridge connection) cannot be set over Bluetooth: put the buddy in the
mode, then stop the bridge.

## Measurements

Capture at least 60 s each, let the board settle for a minute first, and use the PPK software's average over the
window.

1. Display blanked, BLE connected, idle ("Display off"). This is the number for about 21 h per day.
2. Face on: at rest ("Face at rest") and animated ("Face animated"); the web page cannot show many session squares at once,
   so the real worst case is a little higher.
3. Stats and battery pages, and "Everything lit" (every pixel on: the upper bound).
4. BLE advertising with no bridge connected (the state after the laptop sleeps).
5. Optional: system on, no BLE, to isolate the power-management chip and the SoC floor.
6. Buzzer: "Beep once", a short capture of a beep, for the peak. It matters little for the average.

## Calculation

1. Daily charge in mAh = 3 h x I_face + 21 h x I_idle (measured averages).
2. Cell size = daily charge x 7 days x 1.2 (about 20 % margin). Add more if the device will be cold; LiPo capacity
   drops noticeably below about 10 C.
3. Cross-check against the state-of-charge drop per day on the current cell, read from the web page. 1 % of the cell
   capacity is the unit; over a few days the two results should agree within about 15 %.

Example: idle 0.05 mA and face 5 mA give about 16 mAh per day, so about 130 mAh for 7 days with margin.

## Record the results

Put the measured currents in [HARDWARE.md](HARDWARE.md#power-measured).
