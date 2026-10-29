#include <errno.h>

#include "measure_mode.h"

int measure_mode_from_wire(enum measure_mode *mode, const uint8_t *buf, size_t len)
{
	if (len != MEASURE_WRITE_SIZE || buf[0] != MEASURE_FORMAT || buf[1] >= MEASURE_MODE_COUNT) {
		return -EINVAL;
	}
	*mode = (enum measure_mode)buf[1];
	return 0;
}

bool measure_mode_look(enum measure_mode mode, struct measure_look *out)
{
	*out = (struct measure_look){.mood = MOOD_CHILL};
	switch (mode) {
	case MEASURE_DISPLAY_OFF:
		out->blank = true;
		return true;
	case MEASURE_FACE_STILL:
		return true;
	case MEASURE_FACE_ANIMATED:
		out->mood = MOOD_WORKING;
		return true;
	case MEASURE_STATS:
		out->page = 1;
		return true;
	case MEASURE_BATTERY:
		out->page = 2;
		return true;
	case MEASURE_ALL_LIT:
		out->all_pixels = true;
		return true;
	default:
		return false;
	}
}

void measure_mode_to_wire(enum measure_mode mode, uint32_t remaining_ms, uint8_t out[MEASURE_READ_SIZE])
{
	uint32_t minutes = mode == MEASURE_NORMAL ? 0 : (remaining_ms + 59999u) / 60000u;

	out[0] = MEASURE_FORMAT;
	out[1] = (uint8_t)mode;
	out[2] = (uint8_t)(minutes > 255 ? 255 : minutes);
}
