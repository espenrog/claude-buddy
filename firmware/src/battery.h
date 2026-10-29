/*
 * Battery state, its Bluetooth format, and the low-battery levels. Pure logic (no Zephyr), testable on a host.
 * The reading itself comes from a fuel gauge on hardware that has one, or from a test write over Bluetooth.
 */

#ifndef BATTERY_H_
#define BATTERY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BATTERY_WIRE_SIZE 5      /* the test write: format, flags, percent, millivolts */
#define BATTERY_WIRE_SIZE_FULL 9 /* what is read and notified: those five, then battery current, die and cell temperature */
#define BATTERY_FORMAT 1

#define BATTERY_FLAG_PRESENT 0x01
#define BATTERY_FLAG_CHARGING 0x02
#define BATTERY_FLAG_FULL 0x04 /* plugged in and the charge is complete */
#define BATTERY_FLAGS_KNOWN (BATTERY_FLAG_PRESENT | BATTERY_FLAG_CHARGING | BATTERY_FLAG_FULL)

struct battery_state {
	bool present;
	bool charging;
	bool full;
	uint8_t percent; /* 0..100 */
	uint16_t mv;     /* cell voltage in millivolts, 0 = unknown */
	bool power_known; /* current_ma and die_temp_c are valid (a power chip measured them) */
	int16_t current_ma; /* battery current: positive = charging, negative = discharging */
	int8_t die_temp_c;  /* temperature of the power chip */
	bool cell_temp_known; /* cell_temp_c is valid (a thermistor on the cell is connected and read) */
	int8_t cell_temp_c;   /* temperature of the cell, from its thermistor */
};

/* A change in current or temperature smaller than this is not worth waking the display and the radio for. */
#define BATTERY_CURRENT_STEP_MA 10
#define BATTERY_TEMP_STEP_C 2

enum battery_level {
	BATTERY_NONE, /* no battery: nothing to warn about */
	BATTERY_OK,
	BATTERY_LOW,
	BATTERY_CRITICAL,
};

/* Bits of the power chip's (nPM1300) charger status register, as the Zephyr driver reports them. */
#define BATTERY_NPM_STATUS_COMPLETE 0x02
#define BATTERY_NPM_STATUS_TRICKLE 0x04
#define BATTERY_NPM_STATUS_CC 0x08
#define BATTERY_NPM_STATUS_CV 0x10

/* Below this cell voltage there is no battery connected (the chip then reads a floating pin). */
#define BATTERY_PRESENT_MIN_MV 2500

/* Decodes the charger status: `charging` while any charging phase runs, `full` once the charge is complete. */
void battery_charge_from_npm_status(uint32_t status, bool *charging, bool *full);

/* Builds the battery state from a fuel gauge reading (state of charge in percent, may be slightly out of range). */
void battery_state_from_gauge(struct battery_state *out, float soc_percent, uint16_t mv, uint32_t npm_status);

/* Adds the power chip's measurements: battery current in amperes (positive = charging), the chip's die temperature and
 * the cell's temperature in degrees C. Out-of-range values are clamped; NaN, or no battery, leaves the value unknown
 * (the cell temperature is unknown separately: a board may have no thermistor on the cell). */
void battery_state_set_power(struct battery_state *s, float current_a, float die_temp_c, float cell_temp_c);

/* Whether `b` differs from `a` enough to be passed on: any change of the charge state, percent or voltage, but of
 * current and temperature only a change of a few steps (they jitter on every reading). */
bool battery_state_differs(const struct battery_state *a, const struct battery_state *b);

/*
 * Wire format: [format, flags, percent, mv (u16 little endian)], and when read, four more bytes: battery current in mA
 * (i16 little endian, positive = charging), die temperature and cell temperature in degrees C (i8 each); 0x8000 and
 * 0x80 mean unknown.
 * battery_from_wire takes the five-byte form only (test writes) and returns 0 or -EINVAL (and leaves *s alone).
 */
int battery_from_wire(struct battery_state *s, const uint8_t *buf, size_t len);
void battery_to_wire(const struct battery_state *s, uint8_t out[BATTERY_WIRE_SIZE_FULL]);

/* State of charge from the open-circuit voltage of a one-cell lithium polymer battery (0..100). */
uint8_t battery_percent_from_mv(uint16_t mv);

/*
 * The warning level for the new reading. There is no warning while charging. `prev` gives the levels some
 * hysteresis so the warning does not flicker around a threshold.
 */
enum battery_level battery_level_next(enum battery_level prev, const struct battery_state *s);

#endif /* BATTERY_H_ */
