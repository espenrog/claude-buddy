#include <errno.h>

#include "buddy_clock.h"

#define MS_PER_MINUTE 60000LL
#define MS_PER_HOUR (60 * MS_PER_MINUTE)
#define MS_PER_DAY (24 * MS_PER_HOUR)

int buddy_clock_set_from_wire(struct buddy_clock *clock, const uint8_t *buf, size_t len, int64_t uptime_ms)
{
	if (len != BUDDY_CLOCK_WIRE_SIZE || buf[0] != BUDDY_CLOCK_FORMAT || buf[1] > 23 || buf[2] > 59) {
		return -EINVAL;
	}
	clock->known = true;
	clock->minute_of_day_at_set_ms = buf[1] * MS_PER_HOUR + buf[2] * MS_PER_MINUTE;
	clock->uptime_at_set_ms = uptime_ms;
	return 0;
}

int buddy_clock_hour(const struct buddy_clock *clock, int64_t uptime_ms)
{
	if (!clock->known) {
		return -1;
	}
	int64_t now = (clock->minute_of_day_at_set_ms + (uptime_ms - clock->uptime_at_set_ms)) % MS_PER_DAY;

	return (int)(now / MS_PER_HOUR);
}
