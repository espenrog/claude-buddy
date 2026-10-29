/*
 * User settings of the buddy and their 8-byte wire format. Pure logic (no Zephyr), so it can be
 * tested on a host. See docs/PROTOCOL.md for the byte layout.
 */

#ifndef BUDDY_CFG_H_
#define BUDDY_CFG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BUDDY_CFG_WIRE_SIZE 8
#define BUDDY_CFG_FORMAT 1
#define FACE_PROFILE_COUNT 3

#define BUDDY_CFG_FLAG_MUTED 0x01
#define BUDDY_CFG_FLAGS_KNOWN BUDDY_CFG_FLAG_MUTED

struct buddy_cfg {
	uint8_t brightness;       /* OLED contrast while awake, 1..255 */
	uint8_t sleep_brightness; /* contrast while the buddy sleeps; 0 switches the display off */
	uint8_t face_profile;     /* 0..FACE_PROFILE_COUNT-1 */
	uint8_t volume;           /* 0..100, for the buzzer on hardware that has one */
	uint8_t quiet_start_h;    /* 0..23: the buzzer stays silent from here ... */
	uint8_t quiet_end_h;      /* ... until here (may wrap past midnight; equal means never) */
	uint8_t flags;            /* BUDDY_CFG_FLAG_* */
};

void buddy_cfg_defaults(struct buddy_cfg *cfg);

/* Parses and validates the wire format. Returns 0, or -EINVAL (and leaves *cfg alone). */
int buddy_cfg_from_wire(struct buddy_cfg *cfg, const uint8_t *buf, size_t len);

void buddy_cfg_to_wire(const struct buddy_cfg *cfg, uint8_t out[BUDDY_CFG_WIRE_SIZE]);

/*
 * Spending summary the bridge writes to the usage characteristic.
 * Format 1 (firmware 0.4): [1, flags, month_pct].
 * Format 2 (firmware 0.5): [2, flags, month_pct, month_cents (u32 LE), today_cents (u16 LE, saturating)].
 */
#define BUDDY_USAGE_WIRE_SIZE_V1 3
#define BUDDY_USAGE_WIRE_SIZE_V2 9
#define BUDDY_USAGE_WIRE_SIZE_MAX BUDDY_USAGE_WIRE_SIZE_V2
#define BUDDY_USAGE_FLAG_BUDGET 0x01       /* a monthly budget is set, month_pct is meaningful */
#define BUDDY_USAGE_FLAG_OVER_WARNING 0x02 /* spending passed the warning level: dollar-sign eyes */
#define BUDDY_USAGE_FLAGS_KNOWN (BUDDY_USAGE_FLAG_BUDGET | BUDDY_USAGE_FLAG_OVER_WARNING)

struct buddy_usage {
	uint8_t flags;     /* BUDDY_USAGE_FLAG_* */
	uint8_t month_pct; /* month's spending as a percent of the budget, saturating at 255 */
	bool has_amounts;  /* false for format 1: the amounts below are unknown */
	uint32_t month_cents;
	uint16_t today_cents;
};

/* Parses and validates the wire format. Returns 0, or -EINVAL (and leaves *usage alone). */
int buddy_usage_from_wire(struct buddy_usage *usage, const uint8_t *buf, size_t len);

/* Writes format 2 if the amounts are known, else format 1. Returns the number of bytes. */
size_t buddy_usage_to_wire(const struct buddy_usage *usage, uint8_t out[BUDDY_USAGE_WIRE_SIZE_MAX]);

/* True if the given hour (0..23) is inside the quiet hours. */
bool buddy_cfg_is_quiet(const struct buddy_cfg *cfg, int hour);

#endif /* BUDDY_CFG_H_ */
