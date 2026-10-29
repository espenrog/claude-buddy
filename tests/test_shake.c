/* Host-side unit tests for the shake detector. Build and run via tests/test_mood_c.py. */

#include <stdio.h>

#include "shake.h"

static int failures;

#define CHECK(cond)                                                                        \
	do {                                                                               \
		if (!(cond)) {                                                             \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
			failures++;                                                        \
		}                                                                          \
	} while (0)

#define SAMPLE_MS 20

/* Feeds `n` samples of gravity on z plus `swing` milli-g on x that flips sign every `half` samples; counts the shakes. */
static int feed(struct shake_det *det, int64_t *t, int n, int swing, int half)
{
	int shakes = 0;

	for (int i = 0; i < n; i++, *t += SAMPLE_MS) {
		int16_t mg[3] = {(int16_t)(((i / half) & 1) ? -swing : swing), 0, 1000};

		shakes += shake_feed(det, mg, *t);
	}
	return shakes;
}

static void test_resting_is_no_shake(void)
{
	struct shake_det det;
	int64_t t = 0;

	shake_reset(&det);
	CHECK(feed(&det, &t, 200, 0, 5) == 0);
}

static void test_hard_shake_is_detected(void)
{
	struct shake_det det;
	int64_t t = 0;

	shake_reset(&det);
	CHECK(feed(&det, &t, 1, 0, 5) == 0);        /* at rest first */
	CHECK(feed(&det, &t, 25, 2000, 5) >= 1);    /* +-2 g at 5 Hz for half a second */
}

static void test_gentle_movement_is_no_shake(void)
{
	struct shake_det det;
	int64_t t = 0;

	shake_reset(&det);
	CHECK(feed(&det, &t, 1, 0, 5) == 0);
	CHECK(feed(&det, &t, 200, 500, 5) == 0);    /* +-0.5 g: carrying it around */
}

static void test_one_knock_is_no_shake(void)
{
	struct shake_det det;
	int64_t t = 0;
	int shakes = 0;

	shake_reset(&det);
	CHECK(feed(&det, &t, 1, 0, 5) == 0);
	for (int i = 0; i < 100; i++, t += SAMPLE_MS) {
		int16_t mg[3] = {(int16_t)(i == 10 ? 3000 : (i == 11 ? -1500 : 0)), 0, 1000};

		shakes += shake_feed(&det, mg, t);
	}
	CHECK(shakes == 0);
}

static void test_slow_swings_are_no_shake(void)
{
	struct shake_det det;
	int64_t t = 0;
	int shakes = 0;

	shake_reset(&det);
	CHECK(feed(&det, &t, 1, 0, 5) == 0);
	/* Strong swings, but 1 s apart: four of them take longer than the window. */
	for (int i = 0; i < 300; i++, t += SAMPLE_MS) {
		int16_t mg[3] = {(int16_t)(i % 50 == 0 ? ((i / 50) & 1 ? -2500 : 2500) : 0), 0, 1000};

		shakes += shake_feed(&det, mg, t);
	}
	CHECK(shakes == 0);
}

int main(void)
{
	test_resting_is_no_shake();
	test_hard_shake_is_detected();
	test_gentle_movement_is_no_shake();
	test_one_knock_is_no_shake();
	test_slow_swings_are_no_shake();

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all shake tests passed\n");
	return 0;
}
