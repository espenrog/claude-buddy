/*
 * Tells a shake from other movement in a stream of accelerometer samples. Pure logic: time is passed in, so it can be
 * tested on a host without Zephyr. The driver that reads the sensor is motion.c.
 *
 * A shake is the buddy swinging back and forth hard: on some axis the acceleration, with the slow part (gravity, the
 * way it is held) taken away, goes past +SHAKE_SWING_MG and -SHAKE_SWING_MG in turn, SHAKE_SWINGS times within
 * SHAKE_WINDOW_MS. Picking it up, putting it down or a knock on the desk give one swing or two, not that many.
 */

#ifndef SHAKE_H_
#define SHAKE_H_

#include <stdbool.h>
#include <stdint.h>

#define SHAKE_SWING_MG 1000
#define SHAKE_SWINGS 4
#define SHAKE_WINDOW_MS 1500

struct shake_det {
	int32_t slow_mg[3]; /* the slow part of each axis, times 8 (a running average) */
	bool primed;        /* slow_mg holds a value */
	int8_t last_sign;   /* direction of the last strong swing, 0 = none yet */
	uint8_t swings;
	int64_t first_swing_ms;
};

void shake_reset(struct shake_det *det);

/* Feeds one sample (milli-g on each axis). Returns true when it completes a shake; the count then starts again. */
bool shake_feed(struct shake_det *det, const int16_t mg[3], int64_t now_ms);

#endif /* SHAKE_H_ */
