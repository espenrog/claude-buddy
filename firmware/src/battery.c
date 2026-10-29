#include <errno.h>
#include <stdlib.h>

#include "battery.h"

#define CRITICAL_ENTER 10
#define CRITICAL_EXIT 15
#define LOW_ENTER 20
#define LOW_EXIT 25

int battery_from_wire(struct battery_state *s, const uint8_t *buf, size_t len)
{
	if (len != BATTERY_WIRE_SIZE || buf[0] != BATTERY_FORMAT || (buf[1] & ~BATTERY_FLAGS_KNOWN) != 0 ||
	    buf[2] > 100) {
		return -EINVAL;
	}
	uint16_t mv = (uint16_t)(buf[3] | (buf[4] << 8));
	bool present = (buf[1] & BATTERY_FLAG_PRESENT) != 0;
	bool charging = (buf[1] & BATTERY_FLAG_CHARGING) != 0;
	bool full = (buf[1] & BATTERY_FLAG_FULL) != 0;

	if (mv > 5000 || (charging && full) || (!present && (buf[1] != 0 || buf[2] != 0 || mv != 0))) {
		return -EINVAL;
	}
	s->present = present;
	s->charging = charging;
	s->full = full;
	s->percent = buf[2];
	s->mv = mv;
	s->power_known = false; /* a test write carries no current or temperature */
	s->current_ma = 0;
	s->die_temp_c = 0;
	s->cell_temp_known = false;
	s->cell_temp_c = 0;
	return 0;
}

void battery_to_wire(const struct battery_state *s, uint8_t out[BATTERY_WIRE_SIZE_FULL])
{
	int16_t current = s->power_known ? s->current_ma : INT16_MIN;
	int8_t temp = s->power_known ? s->die_temp_c : INT8_MIN;

	out[0] = BATTERY_FORMAT;
	out[1] = (s->present ? BATTERY_FLAG_PRESENT : 0) | (s->charging ? BATTERY_FLAG_CHARGING : 0) |
		 (s->full ? BATTERY_FLAG_FULL : 0);
	out[2] = s->percent;
	out[3] = (uint8_t)s->mv;
	out[4] = (uint8_t)(s->mv >> 8);
	out[5] = (uint8_t)current;
	out[6] = (uint8_t)((uint16_t)current >> 8);
	out[7] = (uint8_t)temp;
	out[8] = s->cell_temp_known ? (uint8_t)s->cell_temp_c : (uint8_t)INT8_MIN;
}

static int8_t celsius_to_i8(float c)
{
	c += c >= 0.0f ? 0.5f : -0.5f; /* round to nearest; the cast truncates */
	return (int8_t)(c > 125.0f ? 125 : c < -40.0f ? -40 : c);
}

void battery_state_set_power(struct battery_state *s, float current_a, float die_temp_c, float cell_temp_c)
{
	s->power_known = false;
	s->current_ma = 0;
	s->die_temp_c = 0;
	s->cell_temp_known = false;
	s->cell_temp_c = 0;
	if (!s->present) {
		return;
	}
	if (cell_temp_c == cell_temp_c) { /* not NaN */
		s->cell_temp_c = celsius_to_i8(cell_temp_c);
		s->cell_temp_known = true;
	}
	if (current_a != current_a || die_temp_c != die_temp_c) {
		return;
	}
	float ma = current_a * 1000.0f;

	ma += ma >= 0.0f ? 0.5f : -0.5f;
	s->current_ma = (int16_t)(ma > 5000.0f ? 5000 : ma < -5000.0f ? -5000 : ma);
	s->die_temp_c = celsius_to_i8(die_temp_c);
	s->power_known = true;
}

bool battery_state_differs(const struct battery_state *a, const struct battery_state *b)
{
	if (a->present != b->present || a->charging != b->charging || a->full != b->full || a->percent != b->percent ||
	    a->mv != b->mv || a->power_known != b->power_known || a->cell_temp_known != b->cell_temp_known) {
		return true;
	}
	return (a->power_known && (abs(a->current_ma - b->current_ma) >= BATTERY_CURRENT_STEP_MA ||
				   abs(a->die_temp_c - b->die_temp_c) >= BATTERY_TEMP_STEP_C)) ||
	       (a->cell_temp_known && abs(a->cell_temp_c - b->cell_temp_c) >= BATTERY_TEMP_STEP_C);
}

void battery_charge_from_npm_status(uint32_t status, bool *charging, bool *full)
{
	*full = (status & BATTERY_NPM_STATUS_COMPLETE) != 0;
	*charging = !*full && (status & (BATTERY_NPM_STATUS_TRICKLE | BATTERY_NPM_STATUS_CC | BATTERY_NPM_STATUS_CV)) != 0;
}

void battery_state_from_gauge(struct battery_state *out, float soc_percent, uint16_t mv, uint32_t npm_status)
{
	if (mv < BATTERY_PRESENT_MIN_MV || mv > 5000) {
		*out = (struct battery_state){0};
		return;
	}
	out->present = true;
	battery_charge_from_npm_status(npm_status, &out->charging, &out->full);
	if (!(soc_percent > 0.0f)) { /* also catches NaN */
		out->percent = 0;
	} else if (soc_percent >= 100.0f) {
		out->percent = 100;
	} else {
		out->percent = (uint8_t)(soc_percent + 0.5f);
	}
	out->mv = mv;
}

uint8_t battery_percent_from_mv(uint16_t mv)
{
	static const struct {
		uint16_t mv;
		uint8_t pct;
	} curve[] = {
		{3270, 0},   {3610, 5},   {3690, 10},  {3710, 15},  {3730, 20},  {3750, 25},  {3770, 30},
		{3790, 35},  {3800, 40},  {3820, 45},  {3840, 50},  {3850, 55},  {3870, 60},  {3910, 65},
		{3950, 70},  {3980, 75},  {4020, 80},  {4080, 85},  {4110, 90},  {4150, 95},  {4200, 100},
	};
	const size_t n = sizeof(curve) / sizeof(curve[0]);

	if (mv <= curve[0].mv) {
		return 0;
	}
	if (mv >= curve[n - 1].mv) {
		return 100;
	}
	for (size_t i = 1; i < n; i++) {
		if (mv <= curve[i].mv) {
			uint32_t span = curve[i].mv - curve[i - 1].mv;
			uint32_t into = mv - curve[i - 1].mv;

			return (uint8_t)(curve[i - 1].pct + (into * (curve[i].pct - curve[i - 1].pct) + span / 2) / span);
		}
	}
	return 100;
}

enum battery_level battery_level_next(enum battery_level prev, const struct battery_state *s)
{
	if (!s->present) {
		return BATTERY_NONE;
	}
	if (s->charging || s->full) {
		return BATTERY_OK;
	}
	if (s->percent <= CRITICAL_ENTER || (prev == BATTERY_CRITICAL && s->percent < CRITICAL_EXIT)) {
		return BATTERY_CRITICAL;
	}
	if (s->percent <= LOW_ENTER ||
	    ((prev == BATTERY_LOW || prev == BATTERY_CRITICAL) && s->percent < LOW_EXIT)) {
		return BATTERY_LOW;
	}
	return BATTERY_OK;
}
