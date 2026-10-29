#include <stdlib.h>
#include <string.h>

#include "shake.h"

void shake_reset(struct shake_det *det)
{
	memset(det, 0, sizeof(*det));
}

bool shake_feed(struct shake_det *det, const int16_t mg[3], int64_t now_ms)
{
	int strongest = 0;

	if (!det->primed) {
		for (int i = 0; i < 3; i++) {
			det->slow_mg[i] = (int32_t)mg[i] * 8;
		}
		det->primed = true;
		return false;
	}

	/* The fast part of the axis that moves most: the sample minus the running average (which then follows by 1/8). */
	for (int i = 0; i < 3; i++) {
		int32_t fast = (int32_t)mg[i] - det->slow_mg[i] / 8;

		det->slow_mg[i] += fast;
		if (abs(fast) > abs(strongest)) {
			strongest = fast;
		}
	}

	if (det->swings > 0 && now_ms - det->first_swing_ms > SHAKE_WINDOW_MS) {
		det->swings = 0;
		det->last_sign = 0;
	}
	if (abs(strongest) < SHAKE_SWING_MG) {
		return false;
	}

	int8_t sign = strongest > 0 ? 1 : -1;

	if (sign == det->last_sign) {
		return false; /* still the same swing */
	}
	if (det->swings == 0) {
		det->first_swing_ms = now_ms;
	}
	det->last_sign = sign;
	if (++det->swings < SHAKE_SWINGS) {
		return false;
	}
	det->swings = 0;
	det->last_sign = 0;
	return true;
}
