/*
 * Measurement modes: hold the buddy in one state (display off, the face at rest, the face animated, a page, everything
 * lit) so its current can be measured with a power profiler. Pure logic (no Zephyr), testable on a host.
 * A mode is held in RAM only and ends by itself after MEASURE_HOLD_MS, so a forgotten mode never lasts.
 */

#ifndef MEASURE_MODE_H_
#define MEASURE_MODE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mood.h"

#define MEASURE_HOLD_MS (30u * 60u * 1000u)
#define MEASURE_FORMAT 1
#define MEASURE_WRITE_SIZE 2 /* [format, mode] */
#define MEASURE_READ_SIZE 3  /* [format, mode, minutes left (rounded up, 0 = normal)] */

enum measure_mode {
	MEASURE_NORMAL, /* no override */
	MEASURE_DISPLAY_OFF,
	MEASURE_FACE_STILL,
	MEASURE_FACE_ANIMATED,
	MEASURE_STATS,
	MEASURE_BATTERY,
	MEASURE_ALL_LIT,
	MEASURE_BEEP, /* not a state: plays one sound now and leaves the current mode alone */
	MEASURE_MODE_COUNT,
};

/* What to show in a held mode. */
struct measure_look {
	bool blank;      /* display off */
	bool all_pixels; /* every pixel on, at full contrast */
	enum mood mood;
	uint8_t page;    /* 0 = face, 1 = stats, 2 = battery (the order of enum ui_page in ui.c) */
};

/* Returns 0 and the mode, or -EINVAL for a wrong length, format or mode. */
int measure_mode_from_wire(enum measure_mode *mode, const uint8_t *buf, size_t len);

/* The look of a held mode (MEASURE_NORMAL and MEASURE_BEEP have none: returns false). */
bool measure_mode_look(enum measure_mode mode, struct measure_look *out);

void measure_mode_to_wire(enum measure_mode mode, uint32_t remaining_ms, uint8_t out[MEASURE_READ_SIZE]);

#endif /* MEASURE_MODE_H_ */
