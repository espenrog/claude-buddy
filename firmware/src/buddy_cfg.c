#include <errno.h>

#include "buddy_cfg.h"

void buddy_cfg_defaults(struct buddy_cfg *cfg)
{
	cfg->brightness = 128;
	cfg->sleep_brightness = 8; /* dim, not off: a dark display looks broken to a new user */
	cfg->face_profile = 0;
	cfg->volume = 60;
	cfg->quiet_start_h = 22;
	cfg->quiet_end_h = 7;
	cfg->flags = 0;
}

int buddy_cfg_from_wire(struct buddy_cfg *cfg, const uint8_t *buf, size_t len)
{
	struct buddy_cfg next;

	if (len != BUDDY_CFG_WIRE_SIZE || buf[0] != BUDDY_CFG_FORMAT) {
		return -EINVAL;
	}

	next.brightness = buf[1];
	next.sleep_brightness = buf[2];
	next.face_profile = buf[3];
	next.volume = buf[4];
	next.quiet_start_h = buf[5];
	next.quiet_end_h = buf[6];
	next.flags = buf[7];

	if (next.brightness == 0 || next.face_profile >= FACE_PROFILE_COUNT || next.volume > 100 ||
	    next.quiet_start_h > 23 || next.quiet_end_h > 23 ||
	    (next.flags & ~BUDDY_CFG_FLAGS_KNOWN) != 0) {
		return -EINVAL;
	}

	*cfg = next;
	return 0;
}

void buddy_cfg_to_wire(const struct buddy_cfg *cfg, uint8_t out[BUDDY_CFG_WIRE_SIZE])
{
	out[0] = BUDDY_CFG_FORMAT;
	out[1] = cfg->brightness;
	out[2] = cfg->sleep_brightness;
	out[3] = cfg->face_profile;
	out[4] = cfg->volume;
	out[5] = cfg->quiet_start_h;
	out[6] = cfg->quiet_end_h;
	out[7] = cfg->flags;
}

int buddy_usage_from_wire(struct buddy_usage *usage, const uint8_t *buf, size_t len)
{
	if (len == 0 || (buf[0] == 1 && len != BUDDY_USAGE_WIRE_SIZE_V1) ||
	    (buf[0] == 2 && len != BUDDY_USAGE_WIRE_SIZE_V2) || (buf[0] != 1 && buf[0] != 2)) {
		return -EINVAL;
	}
	if ((buf[1] & ~BUDDY_USAGE_FLAGS_KNOWN) != 0) {
		return -EINVAL;
	}
	/* Without a budget there is nothing to be over, and no percentage to show. */
	if ((buf[1] & BUDDY_USAGE_FLAG_BUDGET) == 0 && (buf[1] != 0 || buf[2] != 0)) {
		return -EINVAL;
	}
	usage->flags = buf[1];
	usage->month_pct = buf[2];
	usage->has_amounts = (buf[0] == 2);
	usage->month_cents = 0;
	usage->today_cents = 0;
	if (usage->has_amounts) {
		usage->month_cents = (uint32_t)buf[3] | ((uint32_t)buf[4] << 8) |
				     ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 24);
		usage->today_cents = (uint16_t)(buf[7] | (buf[8] << 8));
	}
	return 0;
}

size_t buddy_usage_to_wire(const struct buddy_usage *usage, uint8_t out[BUDDY_USAGE_WIRE_SIZE_MAX])
{
	out[0] = usage->has_amounts ? 2 : 1;
	out[1] = usage->flags;
	out[2] = usage->month_pct;
	if (!usage->has_amounts) {
		return BUDDY_USAGE_WIRE_SIZE_V1;
	}
	out[3] = (uint8_t)(usage->month_cents);
	out[4] = (uint8_t)(usage->month_cents >> 8);
	out[5] = (uint8_t)(usage->month_cents >> 16);
	out[6] = (uint8_t)(usage->month_cents >> 24);
	out[7] = (uint8_t)(usage->today_cents);
	out[8] = (uint8_t)(usage->today_cents >> 8);
	return BUDDY_USAGE_WIRE_SIZE_V2;
}

bool buddy_cfg_is_quiet(const struct buddy_cfg *cfg, int hour)
{
	if (cfg->quiet_start_h == cfg->quiet_end_h) {
		return false;
	}
	if (cfg->quiet_start_h < cfg->quiet_end_h) {
		return hour >= cfg->quiet_start_h && hour < cfg->quiet_end_h;
	}
	return hour >= cfg->quiet_start_h || hour < cfg->quiet_end_h; /* wraps past midnight */
}
