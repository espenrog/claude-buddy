/*
 * Time of day for quiet hours. The buddy has no clock of its own: the bridge writes the local time now and then,
 * and the firmware counts forward from there with its uptime. Pure logic (no Zephyr), testable on a host.
 */

#ifndef BUDDY_CLOCK_H_
#define BUDDY_CLOCK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BUDDY_CLOCK_WIRE_SIZE 3
#define BUDDY_CLOCK_FORMAT 1

struct buddy_clock {
	bool known;
	int64_t minute_of_day_at_set_ms; /* local time of day when it was set, in ms since midnight */
	int64_t uptime_at_set_ms;
};

/* Parses [format, hour, minute]. Returns 0, or -EINVAL (and leaves the clock alone). */
int buddy_clock_set_from_wire(struct buddy_clock *clock, const uint8_t *buf, size_t len, int64_t uptime_ms);

/* Hour of day 0..23, or -1 if the time was never set. */
int buddy_clock_hour(const struct buddy_clock *clock, int64_t uptime_ms);

#endif /* BUDDY_CLOCK_H_ */
