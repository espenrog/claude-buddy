/* Buzzer driver: plays the sound patterns from sound.h on a PWM pin, if the board has one wired up. */

#ifndef BUZZER_H_
#define BUZZER_H_

#include <stdbool.h>

#include "sound.h"

/* Returns 0 also when no buzzer is configured (then buzzer_play does nothing). */
int buzzer_init(void);

bool buzzer_present(void);

/* Queues a sound if it is audible right now (volume, mute and quiet hours); safe from any thread. */
void buzzer_play(enum sound id);

/* Like buzzer_play, but also when muted or in the quiet hours (still at the set volume): for measurements. */
void buzzer_play_always(enum sound id);

/* The time of day from the bridge, for quiet hours. Returns 0, or -EINVAL for a bad value. */
int buzzer_set_time(const uint8_t *wire, size_t len);

/* Hour of day (0..23) from that time, or -1 if the bridge never set it. */
int buzzer_clock_hour(void);

#endif /* BUZZER_H_ */
