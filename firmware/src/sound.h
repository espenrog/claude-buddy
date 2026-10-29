/*
 * What the buzzer plays and when. Pure logic (no Zephyr), so it can be tested on a host.
 * The driver that makes the noise is buzzer.c.
 */

#ifndef SOUND_H_
#define SOUND_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "battery.h"
#include "buddy_cfg.h"
#include "mood.h"

enum sound {
	SOUND_FINISHED, /* a turn finished: short rising chirp */
	SOUND_NEEDS_YOU, /* a session waits for you: two beeps and a high one */
	SOUND_NUDGE,     /* still waiting: one soft, lower double beep */
	SOUND_LIMIT,     /* usage limit reached: falling tones */
	SOUND_UNMUTED,   /* confirms that the buzzer works again */
	SOUND_LOW_BATTERY, /* the battery ran low: three falling beeps */
	SOUND_FALL,      /* dropped: a quick falling whistle ("aaah") */
	SOUND_COUNT,
};

struct tone {
	uint16_t hz; /* 0 = silence */
	uint16_t ms;
};

const struct tone *sound_tones(enum sound id, size_t *count);

/* The sound to play when the mood changes from `from` to `to`, or -1 for none. */
int sound_for_transition(enum mood from, enum mood to);

/* The sound for a battery level change (only when it gets worse), or -1. */
int sound_for_battery(enum battery_level from, enum battery_level to);

/* Milliseconds until the next reminder while a session keeps waiting (`done` reminders were given), 0 = stop. */
uint32_t sound_nudge_delay_ms(unsigned done);

/* Not muted, volume above 0, and not in the quiet hours (hour -1 = time unknown: not quiet). */
bool sound_audible(const struct buddy_cfg *cfg, int hour);

/* PWM duty cycle in percent for a volume of 0..100 (a square wave is loudest at 50 %). */
uint8_t sound_duty_percent(uint8_t volume);

#endif /* SOUND_H_ */
