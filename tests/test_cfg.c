/* Host-side unit tests for the settings wire format. Built and run via tests/test_mood_c.py. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "buddy_cfg.h"

static int failures;

#define CHECK(cond)                                                                        \
	do {                                                                               \
		if (!(cond)) {                                                             \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
			failures++;                                                        \
		}                                                                          \
	} while (0)

static void test_defaults_survive_a_round_trip(void)
{
	struct buddy_cfg a, b;
	uint8_t wire[BUDDY_CFG_WIRE_SIZE];

	buddy_cfg_defaults(&a);
	buddy_cfg_to_wire(&a, wire);
	CHECK(wire[0] == BUDDY_CFG_FORMAT);
	CHECK(buddy_cfg_from_wire(&b, wire, sizeof(wire)) == 0);
	CHECK(memcmp(&a, &b, sizeof(a)) == 0);
}

static void test_every_field_is_carried(void)
{
	const uint8_t wire[BUDDY_CFG_WIRE_SIZE] = {BUDDY_CFG_FORMAT, 200, 0, 2, 75, 23, 6, BUDDY_CFG_FLAG_MUTED};
	struct buddy_cfg c;
	uint8_t out[BUDDY_CFG_WIRE_SIZE];

	CHECK(buddy_cfg_from_wire(&c, wire, sizeof(wire)) == 0);
	CHECK(c.brightness == 200 && c.sleep_brightness == 0 && c.face_profile == 2);
	CHECK(c.volume == 75 && c.quiet_start_h == 23 && c.quiet_end_h == 6);
	CHECK(c.flags == BUDDY_CFG_FLAG_MUTED);
	buddy_cfg_to_wire(&c, out);
	CHECK(memcmp(out, wire, sizeof(wire)) == 0);
}

static void test_invalid_input_is_rejected_and_changes_nothing(void)
{
	struct buddy_cfg c, before;
	uint8_t good[BUDDY_CFG_WIRE_SIZE];
	uint8_t bad[BUDDY_CFG_WIRE_SIZE];

	buddy_cfg_defaults(&c);
	before = c;
	buddy_cfg_to_wire(&c, good);

	CHECK(buddy_cfg_from_wire(&c, good, BUDDY_CFG_WIRE_SIZE - 1) == -EINVAL);
	CHECK(buddy_cfg_from_wire(&c, good, BUDDY_CFG_WIRE_SIZE + 1) == -EINVAL);

	memcpy(bad, good, sizeof(bad)); bad[0] = 9;                 /* unknown format */
	CHECK(buddy_cfg_from_wire(&c, bad, sizeof(bad)) == -EINVAL);
	memcpy(bad, good, sizeof(bad)); bad[1] = 0;                 /* zero brightness */
	CHECK(buddy_cfg_from_wire(&c, bad, sizeof(bad)) == -EINVAL);
	memcpy(bad, good, sizeof(bad)); bad[3] = FACE_PROFILE_COUNT; /* profile out of range */
	CHECK(buddy_cfg_from_wire(&c, bad, sizeof(bad)) == -EINVAL);
	memcpy(bad, good, sizeof(bad)); bad[4] = 101;               /* volume over 100 */
	CHECK(buddy_cfg_from_wire(&c, bad, sizeof(bad)) == -EINVAL);
	memcpy(bad, good, sizeof(bad)); bad[5] = 24;                /* hour out of range */
	CHECK(buddy_cfg_from_wire(&c, bad, sizeof(bad)) == -EINVAL);
	memcpy(bad, good, sizeof(bad)); bad[6] = 200;
	CHECK(buddy_cfg_from_wire(&c, bad, sizeof(bad)) == -EINVAL);
	memcpy(bad, good, sizeof(bad)); bad[7] = 0x80;              /* unknown flag */
	CHECK(buddy_cfg_from_wire(&c, bad, sizeof(bad)) == -EINVAL);

	CHECK(memcmp(&c, &before, sizeof(c)) == 0);
}

static void test_quiet_hours(void)
{
	struct buddy_cfg c;

	buddy_cfg_defaults(&c); /* 22:00 to 07:00, wraps past midnight */
	CHECK(buddy_cfg_is_quiet(&c, 22));
	CHECK(buddy_cfg_is_quiet(&c, 23));
	CHECK(buddy_cfg_is_quiet(&c, 0));
	CHECK(buddy_cfg_is_quiet(&c, 6));
	CHECK(!buddy_cfg_is_quiet(&c, 7));
	CHECK(!buddy_cfg_is_quiet(&c, 12));
	CHECK(!buddy_cfg_is_quiet(&c, 21));

	c.quiet_start_h = 13; c.quiet_end_h = 15; /* a plain afternoon window */
	CHECK(!buddy_cfg_is_quiet(&c, 12));
	CHECK(buddy_cfg_is_quiet(&c, 13));
	CHECK(buddy_cfg_is_quiet(&c, 14));
	CHECK(!buddy_cfg_is_quiet(&c, 15));

	c.quiet_start_h = 9; c.quiet_end_h = 9; /* equal start and end means never quiet */
	for (int h = 0; h < 24; h++) {
		CHECK(!buddy_cfg_is_quiet(&c, h));
	}
}

int main(void)
{
	test_defaults_survive_a_round_trip();
	test_every_field_is_carried();
	test_invalid_input_is_rejected_and_changes_nothing();
	test_quiet_hours();

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all cfg tests passed\n");
	return 0;
}
