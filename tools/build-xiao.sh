#!/bin/sh
# Builds the firmware for the Seeed XIAO nRF54LM20A: MCUboot and the application, signed with your key, updatable over Bluetooth.
# Usage: tools/build-xiao.sh <NCS directory (v3.5.0-preview3 or newer)> <build directory> <signing key .pem>
# Set BUDDY_TOOLCHAIN to a toolchain bundle id (nrfutil sdk-manager list) if the NCS version has no registered toolchain;
# the default is the v3.4.1 bundle that works with the v3.5.0-preview3 tree (see docs/FIRMWARE.md).
# Set BUDDY_BATTERY_MODEL_DIR to a directory holding your own battery_model.inc (made with nPM PowerUP).
# Writes <build directory>/claude-buddy-xiao-all.hex (first flash over USB-C) and dfu_application.zip (updates).
set -eu
ncs="${1:?usage: $0 <NCS directory> <build directory> <signing key .pem>}"; build="${2:?}"; key="${3:?}"
here="$(cd "$(dirname "$0")/.." && pwd)"
case "$build" in /*) ;; *) build="$PWD/$build" ;; esac
case "$key" in /*) ;; *) key="$PWD/$key" ;; esac
[ -f "$key" ] || { echo "no signing key at $key (create one with tools/new-signing-key.sh)" >&2; exit 1; }
[ -z "${BUDDY_BATTERY_MODEL_DIR:-}" ] || export BUDDY_BATTERY_MODEL_DIR
nrfutil sdk-manager toolchain launch --toolchain-bundle-id "${BUDDY_TOOLCHAIN:-fbf7391cab}" --chdir "$ncs" -- \
    west build -p -b xiao_nrf54lm20a/nrf54lm20a/cpuapp -d "$build" "$here/firmware" -- \
    -DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE="\"$key\"" \
    -Dmcuboot_EXTRA_DTC_OVERLAY_FILE="$here/firmware/boards/xiao_nrf54lm20a-mcuboot.overlay"
# Sysbuild writes no merged hex here: join MCUboot and the signed application for the first flash.
exec nrfutil sdk-manager toolchain launch --toolchain-bundle-id "${BUDDY_TOOLCHAIN:-fbf7391cab}" --chdir "$ncs" -- \
    python3 "$ncs/zephyr/scripts/build/mergehex.py" -o "$build/claude-buddy-xiao-all.hex" \
    "$build/mcuboot/zephyr/zephyr.hex" "$build/firmware/zephyr/zephyr.signed.hex"
