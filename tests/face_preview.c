/* Host tool: draws every mood and the stats page as ASCII art. Usage: face_preview [mood 0-10 | name | stats | readme]
 * Built and checked by tests/test_web.py (no overlapping or off-screen drawing crashes), handy for eyeballing changes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "face.h"

static void dump(const uint8_t *fb, const char *title)
{
	printf("--- %s\n", title);
	for (int y = 0; y < FACE_HEIGHT; y++) {
		for (int x = 0; x < FACE_WIDTH; x++) {
			putchar((fb[(y >> 3) * FACE_WIDTH + x] >> (y & 7)) & 1 ? '#' : '.');
		}
		putchar('\n');
	}
}

int main(int argc, char **argv)
{
	static const char *names[] = {"sleeping", "chill", "bored", "working", "excited", "alert", "confused",
				      "angry", "limit", "dizzy", "scared"};
	uint8_t fb[FACE_FB_SIZE];
	struct mood_summary sessions = {.active = 3, .working = 1, .waiting = 1};
	struct buddy_usage none = {0};
	struct buddy_usage over = {.flags = 3, .month_pct = 130, .has_amounts = true, .month_cents = 26012, .today_cents = 1234};
	struct buddy_usage some = {.flags = 1, .month_pct = 42, .has_amounts = true, .month_cents = 8421, .today_cents = 305};
	struct buddy_usage big = {.flags = 0, .has_amounts = true, .month_cents = 123456789, .today_cents = 65535};
	const char *what = argc > 1 ? argv[1] : "all";

	if (strcmp(what, "readme") == 0) {
		/* The pictures in the README (tools/render-faces.py): one session, so no session squares. */
		struct mood_summary one = {.active = 1};
		static const enum mood shown[] = {MOOD_SLEEPING, MOOD_CHILL, MOOD_WORKING, MOOD_EXCITED, MOOD_ALERT,
						  MOOD_CONFUSED, MOOD_ANGRY, MOOD_LIMIT, MOOD_DIZZY, MOOD_SCARED};

		for (size_t i = 0; i < sizeof(shown) / sizeof(shown[0]); i++) {
			face_render(fb, shown[i], 1000, true, &one, 0, &none, BATTERY_OK);
			dump(fb, names[shown[i]]);
		}
		face_render(fb, MOOD_CHILL, 1000, true, &one, 0, &over, BATTERY_OK);
		dump(fb, "dollars");
		face_render(fb, MOOD_CHILL, 1000, false, &one, 0, &none, BATTERY_OK);
		dump(fb, "dead");
		face_render_stats(fb, &some, true);
		dump(fb, "stats");
		return 0;
	}

	for (int m = 0; m <= MOOD_SCARED; m++) {
		if (strcmp(what, "all") != 0 && !(what[0] >= '0' && what[0] <= '9' && atoi(what) == m) &&
		    strcmp(what, names[m]) != 0) {
			continue;
		}
		face_render(fb, (enum mood)m, 1000, true, &sessions, 0, &none, BATTERY_OK);
		dump(fb, names[m]);
	}
	if (strcmp(what, "all") == 0 || strcmp(what, "dollars") == 0) {
		face_render(fb, MOOD_CHILL, 1000, true, &sessions, 0, &over, BATTERY_OK);
		dump(fb, "chill with dollar eyes");
	}
	if (strcmp(what, "all") == 0 || strcmp(what, "dead") == 0) {
		face_render(fb, MOOD_CHILL, 1000, false, &sessions, 0, &none, BATTERY_OK);
		dump(fb, "no BLE link (dead face)");
	}
	if (strcmp(what, "all") == 0 || strcmp(what, "stats") == 0) {
		face_render_stats(fb, &some, true);
		dump(fb, "stats, under budget");
		face_render_stats(fb, &over, true);
		dump(fb, "stats, over budget");
		face_render_stats(fb, &big, false);
		dump(fb, "stats, no budget, big numbers");
		face_render_stats(fb, &none, false);
		dump(fb, "stats, no data");
	}
	if (strcmp(what, "all") == 0 || strcmp(what, "battery") == 0) {
		struct battery_state ok = {.present = true, .percent = 73, .mv = 3916, .power_known = true,
					   .current_ma = -12, .die_temp_c = 27, .cell_temp_known = true, .cell_temp_c = 23};
		struct battery_state low = {.present = true, .percent = 12, .mv = 3700, .power_known = true,
					    .current_ma = -31, .die_temp_c = -5, .cell_temp_known = true, .cell_temp_c = -3};
		struct battery_state chg = {.present = true, .charging = true, .percent = 40, .mv = 3830, .power_known = true,
					    .current_ma = 1120, .die_temp_c = 31, .cell_temp_known = true, .cell_temp_c = 44};
		struct battery_state gone = {0};

		face_render_battery(fb, &ok, BATTERY_OK, true, 0);
		dump(fb, "battery page, fine");
		face_render_battery(fb, &low, BATTERY_LOW, true, 0);
		dump(fb, "battery page, low");
		face_render_battery(fb, &chg, BATTERY_OK, true, 0);
		dump(fb, "battery page, charging");
		face_render_battery(fb, &gone, BATTERY_NONE, false, 0);
		dump(fb, "battery page, no battery");
		face_render(fb, MOOD_CHILL, 0, true, &sessions, 0, &none, BATTERY_LOW);
		face_overlay_text(fb, "UNMUTED");
		dump(fb, "chill, low battery icon, toast");
	}
	return 0;
}
