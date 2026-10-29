/* Host test for the firmware's pure logic: when the picture can change, sounds, the clock. Exit code 0 = all fine. */
#include <stdio.h>
#include <string.h>

#include "battery.h"
#include "buddy_clock.h"
#include "face.h"
#include "sound.h"

static int failures;

#define CHECK(cond)                                                                          \
	do {                                                                                 \
		if (!(cond)) {                                                               \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
			failures++;                                                          \
		}                                                                            \
	} while (0)

static void check_face_timing(void)
{
	static const enum battery_level levels[] = {BATTERY_NONE, BATTERY_LOW, BATTERY_CRITICAL};
	static const struct mood_summary none;
	struct buddy_usage plain = {0};
	struct buddy_usage over = {.flags = BUDDY_USAGE_FLAG_BUDGET | BUDDY_USAGE_FLAG_OVER_WARNING, .month_pct = 120};
	static const enum mood still[] = {MOOD_CHILL, MOOD_ANGRY, MOOD_LIMIT, MOOD_SLEEPING};
	uint8_t a[FACE_FB_SIZE], b[FACE_FB_SIZE];

	for (size_t m = 0; m < sizeof(still) / sizeof(still[0]); m++) {
		for (int variant = 0; variant < 6; variant++) {
			int dollars = variant & 1;
			enum battery_level battery = levels[variant >> 1];
			const struct buddy_usage *usage = dollars ? &over : &plain;

			for (int64_t t = 0; t < 17000; t += 13) {
				uint32_t d = face_next_change_ms(still[m], t, dollars != 0, battery);

				CHECK(d >= 1);
				face_render(a, still[m], t, true, &none, 0, usage, battery);
				const uint32_t probes[] = {1, d / 2, d > 1 ? d - 1 : 1};

				for (size_t i = 0; i < 3; i++) {
					if (probes[i] == 0 || probes[i] >= d) {
						continue;
					}
					face_render(b, still[m], t + probes[i], true, &none, 0, usage, battery);
					if (memcmp(a, b, sizeof(a)) != 0) {
						printf("FAIL mood %d variant %d: picture at %lld ms differs after %u ms (claimed %u)\n",
						       still[m], variant, (long long)t, probes[i], d);
						failures++;
						goto next;
					}
				}
			}
next:
			;
		}
	}
	/* Animated moods are redrawn at the animation rate. */
	CHECK(face_next_change_ms(MOOD_WORKING, 123, false, BATTERY_OK) == FACE_ANIMATED_MS);
	CHECK(face_next_change_ms(MOOD_ALERT, 123, false, BATTERY_OK) == FACE_ANIMATED_MS);
	/* A blink starts at the multiples of 4200 ms. */
	CHECK(face_next_change_ms(MOOD_CHILL, 4100, false, BATTERY_OK) == 100);
	CHECK(face_next_change_ms(MOOD_CHILL, 4200, false, BATTERY_OK) == 140);
	/* A low battery blinks its icon twice a second at most. */
	CHECK(face_next_change_ms(MOOD_CHILL, 4100, false, BATTERY_LOW) == 100);
	CHECK(face_next_change_ms(MOOD_CHILL, 3800, false, BATTERY_LOW) == 200);
}

static void check_sounds(void)
{
	for (int id = 0; id < SOUND_COUNT; id++) {
		size_t n;
		const struct tone *t = sound_tones((enum sound)id, &n);
		unsigned total = 0;

		CHECK(t != NULL && n > 0);
		for (size_t i = 0; i < n; i++) {
			CHECK(t[i].ms > 0);
			CHECK(t[i].hz == 0 || (t[i].hz >= 300 && t[i].hz <= 3000));
			total += t[i].ms;
		}
		CHECK(total < 1200);
		CHECK(t[n - 1].hz != 0); /* ends on a tone, not a rest */
	}
	size_t n;

	CHECK(sound_tones(SOUND_COUNT, &n) == NULL && n == 0);

	CHECK(sound_for_transition(MOOD_WORKING, MOOD_ALERT) == SOUND_NEEDS_YOU);
	CHECK(sound_for_transition(MOOD_ALERT, MOOD_ALERT) == -1);
	CHECK(sound_for_transition(MOOD_WORKING, MOOD_EXCITED) == SOUND_FINISHED);
	CHECK(sound_for_transition(MOOD_EXCITED, MOOD_CHILL) == -1);
	CHECK(sound_for_transition(MOOD_WORKING, MOOD_LIMIT) == SOUND_LIMIT);
	CHECK(sound_for_transition(MOOD_CHILL, MOOD_WORKING) == -1);
	CHECK(sound_for_transition(MOOD_CHILL, MOOD_ANGRY) == -1);
	CHECK(sound_for_transition(MOOD_CHILL, MOOD_SLEEPING) == -1);
	CHECK(sound_for_transition(MOOD_CHILL, MOOD_SCARED) == SOUND_FALL);
	CHECK(sound_for_transition(MOOD_CHILL, MOOD_DIZZY) == -1);

	CHECK(sound_nudge_delay_ms(0) == 60000);
	CHECK(sound_nudge_delay_ms(2) == 300000);
	CHECK(sound_nudge_delay_ms(3) == 0);

	struct buddy_cfg cfg;

	buddy_cfg_defaults(&cfg); /* volume 60, quiet 22:00 to 07:00 */
	CHECK(sound_audible(&cfg, 12));
	CHECK(sound_audible(&cfg, -1)); /* time unknown: do not stay silent */
	CHECK(!sound_audible(&cfg, 23));
	CHECK(!sound_audible(&cfg, 3));
	CHECK(sound_audible(&cfg, 7));
	cfg.flags = BUDDY_CFG_FLAG_MUTED;
	CHECK(!sound_audible(&cfg, 12));
	cfg.flags = 0;
	cfg.volume = 0;
	CHECK(!sound_audible(&cfg, 12));

	CHECK(sound_duty_percent(0) == 0);
	CHECK(sound_duty_percent(60) == 30);
	CHECK(sound_duty_percent(100) == 50);
	CHECK(sound_duty_percent(255) == 50);
}

static void check_battery(void)
{
	struct battery_state s;
	uint8_t wire[BATTERY_WIRE_SIZE_FULL];

	/* Wire format round trip and rejections. */
	const uint8_t good[] = {1, BATTERY_FLAG_PRESENT | BATTERY_FLAG_CHARGING, 73, 0x4c, 0x0f}; /* 3916 mV */
	CHECK(battery_from_wire(&s, good, sizeof(good)) == 0);
	CHECK(s.present && s.charging && !s.full && s.percent == 73 && s.mv == 3916);
	battery_to_wire(&s, wire);
	CHECK(memcmp(wire, good, sizeof(good)) == 0);
	CHECK(!s.power_known && wire[5] == 0x00 && wire[6] == 0x80 && wire[7] == 0x80 && wire[8] == 0x80); /* unknown current and temperatures */
	const uint8_t none[] = {1, 0, 0, 0, 0};
	CHECK(battery_from_wire(&s, none, sizeof(none)) == 0 && !s.present);
	const uint8_t bad[][5] = {{2, 1, 50, 0, 0}, {1, 8, 50, 0, 0}, {1, 1, 101, 0, 0}, {1, 7, 50, 0, 0},
				  {1, 6, 50, 0, 0}, {1, 0, 5, 0, 0}, {1, 0, 0, 1, 0}, {1, 1, 50, 0x89, 0x13}};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		CHECK(battery_from_wire(&s, bad[i], 5) != 0);
	}
	CHECK(battery_from_wire(&s, good, 4) != 0);

	/* Voltage to percent: monotonic, clamped, plausible. */
	unsigned last = 0;
	for (unsigned mv = 2800; mv <= 4400; mv += 5) {
		unsigned p = battery_percent_from_mv((uint16_t)mv);
		CHECK(p >= last && p <= 100);
		last = p;
	}
	CHECK(battery_percent_from_mv(3000) == 0 && battery_percent_from_mv(4300) == 100);
	CHECK(battery_percent_from_mv(3840) == 50);
	CHECK(battery_percent_from_mv(3860) > 55 && battery_percent_from_mv(3860) < 60);

	/* Levels, with hysteresis. */
	struct battery_state b = {.present = true, .percent = 50};
	enum battery_level level = battery_level_next(BATTERY_NONE, &b);
	CHECK(level == BATTERY_OK);
	b.percent = 21;
	CHECK(battery_level_next(level, &b) == BATTERY_OK);
	b.percent = 20;
	level = battery_level_next(level, &b);
	CHECK(level == BATTERY_LOW);
	b.percent = 24;
	CHECK(battery_level_next(level, &b) == BATTERY_LOW); /* hysteresis */
	b.percent = 25;
	CHECK(battery_level_next(level, &b) == BATTERY_OK);
	b.percent = 10;
	level = battery_level_next(level, &b);
	CHECK(level == BATTERY_CRITICAL);
	b.percent = 14;
	CHECK(battery_level_next(level, &b) == BATTERY_CRITICAL);
	b.percent = 15;
	CHECK(battery_level_next(level, &b) == BATTERY_LOW);
	b.percent = 5;
	b.charging = true; /* plugged in: no warning */
	CHECK(battery_level_next(level, &b) == BATTERY_OK);
	b.charging = false;
	b.present = false;
	CHECK(battery_level_next(level, &b) == BATTERY_NONE);

	/* From the power chip's gauge. */
	bool charging, full;

	battery_charge_from_npm_status(0, &charging, &full);
	CHECK(!charging && !full);
	battery_charge_from_npm_status(BATTERY_NPM_STATUS_CC, &charging, &full);
	CHECK(charging && !full);
	battery_charge_from_npm_status(BATTERY_NPM_STATUS_TRICKLE | BATTERY_NPM_STATUS_CV, &charging, &full);
	CHECK(charging && !full);
	battery_charge_from_npm_status(BATTERY_NPM_STATUS_COMPLETE | BATTERY_NPM_STATUS_CV, &charging, &full);
	CHECK(!charging && full);

	battery_state_from_gauge(&s, 73.4f, 3916, BATTERY_NPM_STATUS_CC);
	CHECK(s.present && s.charging && !s.full && s.percent == 73 && s.mv == 3916);
	battery_state_from_gauge(&s, 73.6f, 3916, 0);
	CHECK(s.percent == 74 && !s.charging);
	battery_state_from_gauge(&s, 100.4f, 4180, BATTERY_NPM_STATUS_COMPLETE);
	CHECK(s.percent == 100 && s.full && !s.charging);
	battery_state_from_gauge(&s, -3.0f, 3300, 0);
	CHECK(s.present && s.percent == 0);
	battery_state_from_gauge(&s, (float)(0.0 / 0.0), 3300, 0);
	CHECK(s.present && s.percent == 0);
	battery_state_from_gauge(&s, 50.0f, 300, 0); /* a floating pin: no battery */
	CHECK(!s.present && !s.charging && !s.full && s.percent == 0 && s.mv == 0);
	battery_state_from_gauge(&s, 50.0f, 6000, 0);
	CHECK(!s.present);
	/* ... and the result is always something the wire format accepts. */
	battery_state_from_gauge(&s, 42.0f, 3800, BATTERY_NPM_STATUS_CV);
	battery_to_wire(&s, wire);
	CHECK(battery_from_wire(&s, wire, BATTERY_WIRE_SIZE) == 0);
	CHECK(battery_from_wire(&s, wire, sizeof(wire)) != 0); /* the eight-byte form is read-only */

	/* Battery current and die temperature: wire format, clamping, unknowns. */
	battery_state_from_gauge(&s, 60.0f, 3900, BATTERY_NPM_STATUS_CC);
	battery_state_set_power(&s, 0.4796f, 30.6f, 23.4f);
	CHECK(s.power_known && s.current_ma == 480 && s.die_temp_c == 31 && s.cell_temp_known && s.cell_temp_c == 23);
	battery_to_wire(&s, wire);
	CHECK(wire[5] == 0xe0 && wire[6] == 0x01 && wire[7] == 31 && wire[8] == 23);
	battery_state_set_power(&s, -0.0124f, -4.6f, -0.4f);
	CHECK(s.current_ma == -12 && s.die_temp_c == -5 && s.cell_temp_c == 0);
	battery_to_wire(&s, wire);
	CHECK(wire[5] == 0xf4 && wire[6] == 0xff && wire[7] == 0xfb && wire[8] == 0);
	battery_state_set_power(&s, 90.0f, 400.0f, -90.0f);
	CHECK(s.current_ma == 5000 && s.die_temp_c == 125 && s.cell_temp_c == -40);
	battery_state_set_power(&s, -90.0f, -400.0f, 90.0f);
	CHECK(s.current_ma == -5000 && s.die_temp_c == -40 && s.cell_temp_c == 90);
	battery_state_set_power(&s, (float)(0.0 / 0.0), 20.0f, 21.0f);
	CHECK(!s.power_known && s.cell_temp_known && s.cell_temp_c == 21); /* the cell thermistor stands on its own */
	battery_state_set_power(&s, 0.1f, 20.0f, (float)(0.0 / 0.0));
	CHECK(s.power_known && !s.cell_temp_known);
	battery_to_wire(&s, wire);
	CHECK(wire[8] == 0x80);
	battery_state_from_gauge(&s, 50.0f, 300, 0); /* no battery: nothing to measure */
	battery_state_set_power(&s, 0.1f, 20.0f, 20.0f);
	CHECK(!s.power_known && !s.cell_temp_known);

	/* Small jitter in current and temperature is not passed on, real changes are. */
	struct battery_state a, b2;

	battery_state_from_gauge(&a, 60.0f, 3900, BATTERY_NPM_STATUS_CC);
	b2 = a;
	battery_state_set_power(&a, 0.480f, 30.0f, 24.0f);
	battery_state_set_power(&b2, 0.475f, 31.0f, 25.0f);
	CHECK(!battery_state_differs(&a, &b2));
	battery_state_set_power(&b2, 0.468f, 30.0f, 24.0f);
	CHECK(battery_state_differs(&a, &b2));
	battery_state_set_power(&b2, 0.480f, 32.0f, 24.0f);
	CHECK(battery_state_differs(&a, &b2));
	battery_state_set_power(&b2, 0.480f, 30.0f, 26.0f);
	CHECK(battery_state_differs(&a, &b2));
	battery_state_set_power(&b2, 0.480f, 30.0f, 24.0f);
	b2.percent++;
	CHECK(battery_state_differs(&a, &b2));
	b2 = a;
	b2.power_known = false;
	CHECK(battery_state_differs(&a, &b2));

	/* Sounds only when it gets worse. */
	CHECK(sound_for_battery(BATTERY_OK, BATTERY_LOW) == SOUND_LOW_BATTERY);
	CHECK(sound_for_battery(BATTERY_LOW, BATTERY_CRITICAL) == SOUND_LOW_BATTERY);
	CHECK(sound_for_battery(BATTERY_LOW, BATTERY_OK) == -1);
	CHECK(sound_for_battery(BATTERY_CRITICAL, BATTERY_LOW) == -1);
	CHECK(sound_for_battery(BATTERY_NONE, BATTERY_OK) == -1);
	CHECK(sound_for_battery(BATTERY_LOW, BATTERY_LOW) == -1);
}

static void check_clock(void)
{
	struct buddy_clock c = {0};
	const uint8_t at_2330[] = {1, 23, 30};

	CHECK(buddy_clock_hour(&c, 5000) == -1);
	CHECK(buddy_clock_set_from_wire(&c, at_2330, 3, 1000) == 0);
	CHECK(buddy_clock_hour(&c, 1000) == 23);
	CHECK(buddy_clock_hour(&c, 1000 + 29 * 60000LL) == 23);
	CHECK(buddy_clock_hour(&c, 1000 + 30 * 60000LL) == 0); /* midnight */
	CHECK(buddy_clock_hour(&c, 1000 + (30 + 24 * 60) * 60000LL) == 0); /* a day later */
	CHECK(buddy_clock_hour(&c, 1000 + 7LL * 24 * 3600000 + 3600000) == 0); /* a week and an hour: 00:30 + ... */

	struct buddy_clock keep = c;
	const uint8_t bad[][3] = {{2, 1, 1}, {1, 24, 0}, {1, 0, 60}};

	for (size_t i = 0; i < 3; i++) {
		CHECK(buddy_clock_set_from_wire(&c, bad[i], 3, 0) != 0);
	}
	CHECK(buddy_clock_set_from_wire(&c, at_2330, 2, 0) != 0);
	CHECK(memcmp(&c, &keep, sizeof(c)) == 0); /* rejected writes change nothing */
}

int main(void)
{
	check_face_timing();
	check_sounds();
	check_clock();
	check_battery();
	if (failures == 0) {
		printf("all fine\n");
	}
	return failures != 0;
}
