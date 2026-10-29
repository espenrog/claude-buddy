/* Host-side unit tests for the measurement modes. Built and run via tests/test_mood_c.py. */

#include <errno.h>
#include <stdio.h>

#include "measure_mode.h"

static int failures;

#define CHECK(cond)                                                                        \
	do {                                                                               \
		if (!(cond)) {                                                             \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
			failures++;                                                        \
		}                                                                          \
	} while (0)

static void test_wire_in(void)
{
	enum measure_mode m = MEASURE_NORMAL;

	CHECK(measure_mode_from_wire(&m, (const uint8_t[]){1, MEASURE_DISPLAY_OFF}, 2) == 0 && m == MEASURE_DISPLAY_OFF);
	CHECK(measure_mode_from_wire(&m, (const uint8_t[]){1, MEASURE_BEEP}, 2) == 0 && m == MEASURE_BEEP);
	CHECK(measure_mode_from_wire(&m, (const uint8_t[]){1, MEASURE_NORMAL}, 2) == 0 && m == MEASURE_NORMAL);
	m = MEASURE_STATS;
	CHECK(measure_mode_from_wire(&m, (const uint8_t[]){1, MEASURE_MODE_COUNT}, 2) == -EINVAL && m == MEASURE_STATS);
	CHECK(measure_mode_from_wire(&m, (const uint8_t[]){2, 1}, 2) == -EINVAL);
	CHECK(measure_mode_from_wire(&m, (const uint8_t[]){1}, 1) == -EINVAL);
	CHECK(measure_mode_from_wire(&m, (const uint8_t[]){1, 1, 0}, 3) == -EINVAL);
}

static void test_looks(void)
{
	struct measure_look l;

	CHECK(!measure_mode_look(MEASURE_NORMAL, &l));
	CHECK(!measure_mode_look(MEASURE_BEEP, &l));
	CHECK(measure_mode_look(MEASURE_DISPLAY_OFF, &l) && l.blank && !l.all_pixels);
	CHECK(measure_mode_look(MEASURE_FACE_STILL, &l) && !l.blank && l.mood == MOOD_CHILL && l.page == 0);
	CHECK(measure_mode_look(MEASURE_FACE_ANIMATED, &l) && l.mood == MOOD_WORKING && l.page == 0);
	CHECK(measure_mode_look(MEASURE_STATS, &l) && l.page == 1 && !l.blank);
	CHECK(measure_mode_look(MEASURE_BATTERY, &l) && l.page == 2);
	CHECK(measure_mode_look(MEASURE_ALL_LIT, &l) && l.all_pixels && !l.blank);
}

static void test_wire_out(void)
{
	uint8_t w[MEASURE_READ_SIZE];

	measure_mode_to_wire(MEASURE_NORMAL, 12345, w);
	CHECK(w[0] == 1 && w[1] == 0 && w[2] == 0);
	measure_mode_to_wire(MEASURE_STATS, MEASURE_HOLD_MS, w);
	CHECK(w[1] == MEASURE_STATS && w[2] == 30);
	measure_mode_to_wire(MEASURE_STATS, 1, w);
	CHECK(w[2] == 1); /* rounded up: never 0 while a mode is held */
	measure_mode_to_wire(MEASURE_STATS, 0, w);
	CHECK(w[2] == 0);
}

int main(void)
{
	test_wire_in();
	test_looks();
	test_wire_out();
	if (failures == 0) {
		printf("all measure mode tests passed\n");
	}
	return failures != 0;
}
