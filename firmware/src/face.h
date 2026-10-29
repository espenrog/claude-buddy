/* Draws the companion's face into a 128x64 1-bit page-tiled framebuffer. */

#ifndef FACE_H_
#define FACE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "battery.h"
#include "buddy_cfg.h"
#include "mood.h"

#define FACE_WIDTH 128
#define FACE_HEIGHT 64
#define FACE_FB_SIZE ((FACE_WIDTH * FACE_HEIGHT) / 8)

/* How long until the picture for `mood` can change again (so the display need not be refreshed before then). */
#define FACE_ANIMATED_MS 50
#define FACE_STILL_MS 1000
uint32_t face_next_change_ms(enum mood mood, int64_t t_ms, bool dollars, enum battery_level battery);

/* Renders one frame for the given mood at animation time t_ms. */
/* `profile` selects the eye style (0 bars, 1 rounded, 2 rounded with a highlight). */
void face_render(uint8_t *fb, enum mood mood, int64_t t_ms, bool connected,
		 const struct mood_summary *sessions, uint8_t profile,
		 const struct buddy_usage *usage, enum battery_level battery);

/* The stats page: this month's estimated spending, today, and the budget bar. */
void face_render_stats(uint8_t *fb, const struct buddy_usage *usage, bool connected);

/* The battery page: an icon filled to the charge, the percentage, and what it is doing. */
void face_render_battery(uint8_t *fb, const struct battery_state *battery, enum battery_level level,
			 bool connected, int64_t t_ms);

/* How long until the battery page can change (the low warning blinks). */
uint32_t face_battery_next_change_ms(enum battery_level level, int64_t t_ms);

/* A message in a box in the middle of whatever is on screen, for example "MUTED". */
void face_overlay_text(uint8_t *fb, const char *text);

#endif /* FACE_H_ */
