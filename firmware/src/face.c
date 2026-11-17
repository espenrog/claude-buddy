#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "face.h"

#define EYE_L_X 40
#define EYE_R_X 88
#define EYE_Y 28
#define EYE_W 16
#define EYE_H 24

#define BLINK_PERIOD_MS 4200
#define BLINK_LEN_MS 140

static uint8_t style; /* eye profile for the frame being drawn */

static void clear_pixel(uint8_t *fb, int x, int y)
{
	if (x < 0 || x >= FACE_WIDTH || y < 0 || y >= FACE_HEIGHT) {
		return;
	}
	fb[(y >> 3) * FACE_WIDTH + x] &= (uint8_t)~(1U << (y & 7));
}

static void put_pixel(uint8_t *fb, int x, int y)
{
	if (x < 0 || x >= FACE_WIDTH || y < 0 || y >= FACE_HEIGHT) {
		return;
	}
	fb[(y >> 3) * FACE_WIDTH + x] |= (uint8_t)(1U << (y & 7));
}

static void fill_rect(uint8_t *fb, int x, int y, int w, int h)
{
	for (int j = 0; j < h; j++) {
		for (int i = 0; i < w; i++) {
			put_pixel(fb, x + i, y + j);
		}
	}
}

static void thick_line(uint8_t *fb, int x0, int y0, int x1, int y1, int thick)
{
	int dx = abs(x1 - x0);
	int dy = -abs(y1 - y0);
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;

	for (;;) {
		fill_rect(fb, x0 - thick / 2, y0 - thick / 2, thick, thick);
		if (x0 == x1 && y0 == y1) {
			break;
		}
		int e2 = 2 * err;

		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}

/* Triangle wave in [0, range] with the given period. */
static int tri(int64_t t_ms, int period_ms, int range)
{
	int p = (int)(t_ms % period_ms) * 2;

	if (p > period_ms) {
		p = 2 * period_ms - p;
	}
	return p * range / period_ms;
}

/* The low-battery icon in the corner blinks: shown for the first half of every second. */
static bool low_icon_on(int64_t t_ms)
{
	return (t_ms % 1000) < 500;
}

static uint32_t until_low_icon_toggle(int64_t t_ms)
{
	int phase = (int)(t_ms % 500);

	return (uint32_t)(500 - phase);
}

static void eye(uint8_t *fb, int cx, int cy, int w, int h)
{
	int x = cx - w / 2;
	int y = cy - h / 2;

	fill_rect(fb, x, y, w, h);
	if (style >= 1 && w >= 8 && h >= 8) { /* rounded corners */
		for (int i = 0; i < 2; i++) {
			for (int j = 0; j < 2 - i; j++) {
				clear_pixel(fb, x + i, y + j);
				clear_pixel(fb, x + w - 1 - i, y + j);
				clear_pixel(fb, x + i, y + h - 1 - j);
				clear_pixel(fb, x + w - 1 - i, y + h - 1 - j);
			}
		}
	}
	if (style >= 2 && w >= 10 && h >= 12) { /* highlight, like a retro game character */
		for (int j = 0; j < 3; j++) {
			for (int i = 0; i < 3; i++) {
				clear_pixel(fb, x + 3 + i, y + 3 + j);
			}
		}
	}
}

static void chevron(uint8_t *fb, int cx, int cy, bool points_right)
{
	int tip = points_right ? cx + 10 : cx - 10;
	int back = points_right ? cx - 12 : cx + 12;

	thick_line(fb, back, cy - 14, tip, cy, 5);
	thick_line(fb, tip, cy, back, cy + 14, 5);
}

/* A dollar sign instead of an eye: an S from three bars and two posts, with a stroke through it. */
static void dollar(uint8_t *fb, int cx, int cy)
{
	fill_rect(fb, cx - 6, cy - 10, 12, 3);
	fill_rect(fb, cx - 6, cy - 10, 3, 11);
	fill_rect(fb, cx - 6, cy - 2, 12, 3);
	fill_rect(fb, cx + 3, cy - 2, 3, 11);
	fill_rect(fb, cx - 6, cy + 7, 12, 3);
	fill_rect(fb, cx - 1, cy - 12, 2, 24);
}

/* An X instead of an eye. */
static void cross(uint8_t *fb, int cx, int cy)
{
	thick_line(fb, cx - 9, cy - 9, cx + 9, cy + 9, 4);
	thick_line(fb, cx - 9, cy + 9, cx + 9, cy - 9, 4);
}

static void draw_dollar_eyes(uint8_t *fb, int dx, int dy)
{
	dollar(fb, EYE_L_X + dx, EYE_Y + dy);
	dollar(fb, EYE_R_X + dx, EYE_Y + dy);
}

static bool blinking(int64_t t_ms)
{
	return (t_ms % BLINK_PERIOD_MS) < BLINK_LEN_MS;
}

static void draw_eyes_open(uint8_t *fb, int dx, int dy, int h, int64_t t_ms)
{
	if (blinking(t_ms)) {
		h = 2;
	}
	eye(fb, EYE_L_X + dx, EYE_Y + dy, EYE_W, h);
	eye(fb, EYE_R_X + dx, EYE_Y + dy, EYE_W, h);
}

static void draw_angry(uint8_t *fb)
{
	for (int i = 0; i < EYE_W; i++) {
		int trim_l = i * 12 / EYE_W;
		int trim_r = (EYE_W - 1 - i) * 12 / EYE_W;

		fill_rect(fb, EYE_L_X - EYE_W / 2 + i, EYE_Y - EYE_H / 2 + trim_l, 1,
			  EYE_H - trim_l);
		fill_rect(fb, EYE_R_X - EYE_W / 2 + i, EYE_Y - EYE_H / 2 + trim_r, 1,
			  EYE_H - trim_r);
	}
}

/* sin() at 32 steps per turn, times 64 (no maths library on the host tests or in the firmware). */
static const int8_t sin32[32] = {0,   12,  24,  36,  45,  53,  59,  63,  64,  63,  59,  53,  45,  36,  24,  12,
				 0,   -12, -24, -36, -45, -53, -59, -63, -64, -63, -59, -53, -45, -36, -24, -12};

/* A spiral of two and a half turns, rotated by `phase` (0..31): a dizzy eye. */
static void spiral(uint8_t *fb, int cx, int cy, int phase)
{
	int px = cx;
	int py = cy;

	for (int k = 1; k <= 40; k++) {
		int a = (k * 2 + phase) & 31;
		int r = k / 4;
		int x = cx + r * sin32[(a + 8) & 31] / 64;
		int y = cy + r * sin32[a] / 64;

		thick_line(fb, px, py, x, y, 2);
		px = x;
		py = y;
	}
}

/* Dropped: wide open hollow eyes with tiny pupils, trembling. */
static void draw_scared(uint8_t *fb, int64_t t_ms)
{
	int dx = ((t_ms / 50) & 1) ? 1 : -1;

	for (int e = 0; e < 2; e++) {
		int cx = (e == 0 ? EYE_L_X : EYE_R_X) + dx;
		int x = cx - 10;
		int y = EYE_Y - 13;

		fill_rect(fb, x, y, 20, 2);
		fill_rect(fb, x, y + 24, 20, 2);
		fill_rect(fb, x, y, 2, 26);
		fill_rect(fb, x + 18, y, 2, 26);
		fill_rect(fb, cx - 2, EYE_Y - 2, 4, 4);
	}
}

static void draw_limit(uint8_t *fb, int64_t t_ms)
{
	fill_rect(fb, 0, 0, FACE_WIDTH, 2);
	fill_rect(fb, 0, FACE_HEIGHT - 2, FACE_WIDTH, 2);
	fill_rect(fb, 0, 0, 2, FACE_HEIGHT);
	fill_rect(fb, FACE_WIDTH - 2, 0, 2, FACE_HEIGHT);

	if ((t_ms % 1000) < 600) {
		fill_rect(fb, 61, 10, 6, 30);
		fill_rect(fb, 61, 46, 6, 6);
	}
}

/* ---- mouths: one small shape under the eyes that follows the mood ---- */

#define MOUTH_X 64
#define MOUTH_Y 50

static void mouth_line(uint8_t *fb, int x0, int y0, int x1, int y1)
{
	thick_line(fb, MOUTH_X + x0, MOUTH_Y + y0, MOUTH_X + x1, MOUTH_Y + y1, 2);
}

/* A filled half disc, flat side up: a wide open grin. */
static void mouth_grin(uint8_t *fb)
{
	static const uint8_t half_width[] = {13, 12, 11, 9, 7, 4};

	for (int row = 0; row < (int)sizeof(half_width); row++) {
		fill_rect(fb, MOUTH_X - half_width[row], MOUTH_Y - 4 + row, 2 * half_width[row], 1);
	}
}

/* No BLE link: crossed-out eyes and a flat mouth. */
static void draw_dead_face(uint8_t *fb)
{
	cross(fb, EYE_L_X, EYE_Y);
	cross(fb, EYE_R_X, EYE_Y);
	mouth_line(fb, -8, 0, 8, 0);
}

static void draw_mouth(uint8_t *fb, enum mood mood, bool grin, int64_t t_ms)
{
	if (grin && (mood == MOOD_CHILL || mood == MOOD_BORED || mood == MOOD_WORKING)) {
		mouth_grin(fb); /* dollar-sign eyes get a money-grabbing grin */
		return;
	}
	switch (mood) {
	case MOOD_SLEEPING:
		mouth_line(fb, -3, 1, 3, 1); /* tiny, relaxed */
		break;
	case MOOD_CHILL: /* gentle smile */
		mouth_line(fb, -10, -3, -5, 1);
		mouth_line(fb, -5, 1, 5, 1);
		mouth_line(fb, 5, 1, 10, -3);
		break;
	case MOOD_BORED: /* flat */
		mouth_line(fb, -7, 0, 7, 0);
		break;
	case MOOD_WORKING: /* concentrating: small and straight */
		mouth_line(fb, -4, 0, 4, 0);
		break;
	case MOOD_EXCITED:
		mouth_grin(fb);
		break;
	case MOOD_ALERT: /* open "o" */
		fill_rect(fb, MOUTH_X - 5, MOUTH_Y - 4, 10, 2);
		fill_rect(fb, MOUTH_X - 5, MOUTH_Y + 4, 10, 2);
		fill_rect(fb, MOUTH_X - 5, MOUTH_Y - 4, 2, 10);
		fill_rect(fb, MOUTH_X + 3, MOUTH_Y - 4, 2, 10);
		break;
	case MOOD_CONFUSED: /* wavy */
		mouth_line(fb, -12, 1, -6, -2);
		mouth_line(fb, -6, -2, 0, 2);
		mouth_line(fb, 0, 2, 6, -2);
		mouth_line(fb, 6, -2, 12, 1);
		break;
	case MOOD_ANGRY: /* frown */
		mouth_line(fb, -10, 4, -5, 0);
		mouth_line(fb, -5, 0, 5, 0);
		mouth_line(fb, 5, 0, 10, 4);
		break;
	case MOOD_LIMIT:
		break; /* the frame and the "!" are the whole picture */
	case MOOD_DIZZY: { /* a wave that rolls from side to side */
		int w = ((t_ms / 150) & 1) ? 2 : -2;

		mouth_line(fb, -12, 0, -6, -w);
		mouth_line(fb, -6, -w, 0, w);
		mouth_line(fb, 0, w, 6, -w);
		mouth_line(fb, 6, -w, 12, 0);
		break;
	}
	case MOOD_SCARED: /* a tall open "O" */
		fill_rect(fb, MOUTH_X - 5, MOUTH_Y - 6, 10, 2);
		fill_rect(fb, MOUTH_X - 5, MOUTH_Y + 6, 10, 2);
		fill_rect(fb, MOUTH_X - 6, MOUTH_Y - 5, 2, 12);
		fill_rect(fb, MOUTH_X + 4, MOUTH_Y - 5, 2, 12);
		break;
	}
}

/* ---- text: a 5x7 font (digits, capitals and a few symbols), for the stats page ---- */

static const struct {
	char ch;
	uint8_t col[5]; /* columns, bit 0 = top row */
} glyphs[] = {
	{' ', {0x00, 0x00, 0x00, 0x00, 0x00}}, {'$', {0x24, 0x2A, 0x7F, 0x2A, 0x12}}, {'+', {0x08, 0x08, 0x3E, 0x08, 0x08}},
	{'%', {0x23, 0x13, 0x08, 0x64, 0x62}}, {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
	{'.', {0x00, 0x60, 0x60, 0x00, 0x00}}, {'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
	{'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}}, {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
	{'2', {0x42, 0x61, 0x51, 0x49, 0x46}}, {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
	{'4', {0x18, 0x14, 0x12, 0x7F, 0x10}}, {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
	{'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}}, {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
	{'8', {0x36, 0x49, 0x49, 0x49, 0x36}}, {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
	{'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}}, {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
	{'C', {0x3E, 0x41, 0x41, 0x41, 0x22}}, {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
	{'E', {0x7F, 0x49, 0x49, 0x49, 0x41}}, {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
	{'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}}, {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
	{'I', {0x00, 0x41, 0x7F, 0x41, 0x00}}, {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
	{'K', {0x7F, 0x08, 0x14, 0x22, 0x41}}, {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
	{'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}}, {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
	{'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}}, {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
	{'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}}, {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
	{'S', {0x46, 0x49, 0x49, 0x49, 0x31}}, {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
	{'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}}, {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
	{'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}}, {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
	{'Y', {0x07, 0x08, 0x70, 0x08, 0x07}}, {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
};

static int text_width(const char *str, int scale)
{
	int n = (int)strlen(str);

	return n == 0 ? 0 : n * 6 * scale - scale;
}

static void draw_text(uint8_t *fb, int x, int y, const char *str, int scale)
{
	for (; *str != '\0'; str++, x += 6 * scale) {
		for (size_t g = 0; g < sizeof(glyphs) / sizeof(glyphs[0]); g++) {
			if (glyphs[g].ch != *str) {
				continue;
			}
			for (int c = 0; c < 5; c++) {
				for (int r = 0; r < 7; r++) {
					if ((glyphs[g].col[c] >> r) & 1) {
						fill_rect(fb, x + c * scale, y + r * scale, scale, scale);
					}
				}
			}
			break;
		}
	}
}

/* "$12.34" (or "$1234" from $1000, "$99999" at most) from cents. */
static void format_dollars(char *out, size_t size, uint32_t cents)
{
	uint32_t dollars = cents / 100;

	if (dollars >= 1000) {
		snprintf(out, size, "$%lu", (unsigned long)(dollars > 99999 ? 99999 : dollars));
	} else {
		snprintf(out, size, "$%lu.%02lu", (unsigned long)dollars, (unsigned long)(cents % 100));
	}
}

void face_render_stats(uint8_t *fb, const struct buddy_usage *usage, bool connected)
{
	char line[24];
	char money[12];

	memset(fb, 0, FACE_FB_SIZE);
	if (connected) {
		fill_rect(fb, FACE_WIDTH - 6, 3, 3, 3);
	}
	draw_text(fb, 4, 2, "EST. THIS MONTH", 1);

	if (!usage->has_amounts) {
		draw_text(fb, 4, 24, "NO DATA YET", 1);
		return;
	}

	format_dollars(money, sizeof(money), usage->month_cents);
	draw_text(fb, 4, 13, money, text_width(money, 3) <= 120 ? 3 : 2);

	format_dollars(money, sizeof(money), usage->today_cents);
	snprintf(line, sizeof(line), "TODAY %s", money);
	draw_text(fb, 4, 36, line, 1);

	if ((usage->flags & BUDDY_USAGE_FLAG_BUDGET) != 0) {
		int filled = (usage->month_pct > 100 ? 100 : usage->month_pct) * 118 / 100;

		fill_rect(fb, 4, 46, 120, 1);
		fill_rect(fb, 4, 53, 120, 1);
		fill_rect(fb, 4, 46, 1, 8);
		fill_rect(fb, 123, 46, 1, 8);
		fill_rect(fb, 5, 48, filled, 4);
		snprintf(line, sizeof(line), "%u%% OF BUDGET", usage->month_pct);
		draw_text(fb, 4, 56, line, 1);
		if ((usage->flags & BUDDY_USAGE_FLAG_OVER_WARNING) != 0) {
			draw_text(fb, 128 - 4 - text_width("OVER", 1), 36, "OVER", 1);
		}
	} else {
		draw_text(fb, 4, 50, "NO BUDGET SET", 1);
	}
}

/* A battery outline with a nub on the right, filled `percent` percent from the left. */
static void draw_battery_icon(uint8_t *fb, int x, int y, int w, int h, int percent)
{
	int inner_w = w - 4;
	int filled = inner_w * percent / 100;

	fill_rect(fb, x, y, w, 1);
	fill_rect(fb, x, y + h - 1, w, 1);
	fill_rect(fb, x, y, 1, h);
	fill_rect(fb, x + w - 1, y, 1, h);
	fill_rect(fb, x + w, y + h / 3, 2, h - 2 * (h / 3));
	if (percent > 0 && filled == 0) {
		filled = 1;
	}
	fill_rect(fb, x + 2, y + 2, filled, h - 4);
}

uint32_t face_battery_next_change_ms(enum battery_level level, int64_t t_ms)
{
	return level >= BATTERY_LOW ? until_low_icon_toggle(t_ms) : FACE_STILL_MS;
}

void face_render_battery(uint8_t *fb, const struct battery_state *battery, enum battery_level level,
			 bool connected, int64_t t_ms)
{
	char text[16];

	memset(fb, 0, FACE_FB_SIZE);
	if (connected) {
		fill_rect(fb, FACE_WIDTH - 6, 3, 3, 3);
	}
	draw_text(fb, 4, 2, "BATTERY", 1);

	if (!battery->present) {
		draw_text(fb, 4, 26, "NO BATTERY", 2);
		return;
	}
	if (battery->mv != 0) {
		snprintf(text, sizeof(text), "%u.%02uV", battery->mv / 1000, (battery->mv % 1000) / 10);
		draw_text(fb, FACE_WIDTH - 12 - text_width(text, 1), 2, text, 1);
	}

	/* Big icon: a low battery blinks its outline's contents so it is noticed. */
	bool hide_fill = level >= BATTERY_LOW && !low_icon_on(t_ms);

	draw_battery_icon(fb, 6, 13, 76, 24, hide_fill ? 0 : battery->percent);
	snprintf(text, sizeof(text), "%u%%", battery->percent);
	draw_text(fb, 92, 18, text, 2);

	/* The battery current (+ = charging), then the cell's temperature and the power chip's, when they are known. */
	if (battery->power_known) {
		snprintf(text, sizeof(text), "%+dMA", battery->current_ma);
		draw_text(fb, 4, 40, text, 1);
		snprintf(text, sizeof(text), "DIE %dC", battery->die_temp_c);
		draw_text(fb, FACE_WIDTH - 4 - text_width(text, 1), 48, text, 1);
	}
	if (battery->cell_temp_known) {
		snprintf(text, sizeof(text), "CELL %dC", battery->cell_temp_c);
		draw_text(fb, 4, 48, text, 1);
	}

	const char *status = battery->full ? "CHARGED" : battery->charging ? "CHARGING"
			     : level == BATTERY_CRITICAL ? "CRITICAL - PLUG IN"
			     : level == BATTERY_LOW ? "LOW - PLUG IN" : "ON BATTERY";

	draw_text(fb, 4, 57, status, 1);
}

void face_overlay_text(uint8_t *fb, const char *text)
{
	int w = text_width(text, 2);
	int box_w = w + 16;
	int x = (FACE_WIDTH - box_w) / 2;

	for (int j = 0; j < 30; j++) {
		for (int i = 0; i < box_w; i++) {
			clear_pixel(fb, x + i, 17 + j);
		}
	}
	fill_rect(fb, x, 17, box_w, 2);
	fill_rect(fb, x, 17 + 28, box_w, 2);
	fill_rect(fb, x, 17, 2, 30);
	fill_rect(fb, x + box_w - 2, 17, 2, 30);
	draw_text(fb, (FACE_WIDTH - w) / 2, 25, text, 2);
}

/*
 * One small square per Claude session, top left, shown only when two or more are running:
 * blinking = waiting for you, solid = working, hollow = idle.
 */
static void draw_sessions(uint8_t *fb, const struct mood_summary *sum, int64_t t_ms)
{
	if (sum->active < 2) {
		return;
	}

	for (int i = 0; i < sum->active; i++) {
		int x = 3 + i * 6;
		int y = 3;

		if (i < sum->waiting) {
			if ((t_ms / 250) & 1) {
				fill_rect(fb, x, y, 4, 4);
			}
		} else if (i < sum->waiting + sum->working) {
			fill_rect(fb, x, y, 4, 4);
		} else {
			fill_rect(fb, x, y, 4, 1);
			fill_rect(fb, x, y + 3, 4, 1);
			fill_rect(fb, x, y + 1, 1, 2);
			fill_rect(fb, x + 3, y + 1, 1, 2);
		}
	}
}

/* ---- animal eye profiles: eyes only, no mouth (the printed beak or teeth is the mouth) ---- */

#define PROFILE_CROW 3
#define PROFILE_BEAVER 4

static void clear_rect(uint8_t *fb, int x, int y, int w, int h)
{
	for (int j = 0; j < h; j++) {
		for (int i = 0; i < w; i++) {
			clear_pixel(fb, x + i, y + j);
		}
	}
}

/* Filled ellipse with radii rx, ry (a pixel is in when its centre is within rx + 0.5, ry + 0.5); `on` false clears. */
static void ellipse(uint8_t *fb, int cx, int cy, int rx, int ry, bool on)
{
	int ax = (2 * rx + 1) * (2 * rx + 1);
	int by = (2 * ry + 1) * (2 * ry + 1);

	for (int y = -ry; y <= ry; y++) {
		for (int x = -rx; x <= rx; x++) {
			if (4 * x * x * by + 4 * y * y * ax <= ax * by) {
				if (on) {
					put_pixel(fb, cx + x, cy + y);
				} else {
					clear_pixel(fb, cx + x, cy + y);
				}
			}
		}
	}
}

static void ring(uint8_t *fb, int cx, int cy, int r, int t)
{
	ellipse(fb, cx, cy, r, r, true);
	ellipse(fb, cx, cy, r - t, r - t, false);
}

/* sin() at 64 steps per turn, times 64, from the 32-step table. */
static int sin64(int a)
{
	a &= 63;
	return (a & 1) ? (sin32[a >> 1] + sin32[((a >> 1) + 1) & 31]) / 2 : sin32[a >> 1];
}

/* Elliptic arc from a0 to a1 in 1/64 turns (0 = right, 16 = down), drawn with a thick line. */
static void arc(uint8_t *fb, int cx, int cy, int rx, int ry, int a0, int a1, int thick)
{
	int px = cx + rx * sin64(a0 + 16) / 64;
	int py = cy + ry * sin64(a0) / 64;

	for (int a = a0 + 1; a <= a1; a++) {
		int x = cx + rx * sin64(a + 16) / 64;
		int y = cy + ry * sin64(a) / 64;

		thick_line(fb, px, py, x, y, thick);
		px = x;
		py = y;
	}
}

/* A small "z" for sleeping, s pixels per stroke. */
static void zee(uint8_t *fb, int x, int y, int s)
{
	fill_rect(fb, x, y, 3 * s, s);
	thick_line(fb, x + 3 * s - 1, y + s, x, y + 4 * s - 1, s);
	fill_rect(fb, x, y + 4 * s, 3 * s, s);
}

static void invert_fb(uint8_t *fb)
{
	for (int i = 0; i < FACE_FB_SIZE; i++) {
		fb[i] = (uint8_t)~fb[i];
	}
}

/* Crow (Hugin): beady round eyes with a highlight under sharp, slightly smug brows. */
#define CROW_L 40
#define CROW_R 88
#define CROW_Y 32
#define CROW_EYE_R 10

/* slant > 0 puts the inner ends lower (smug), < 0 raises them; lift moves both brows up. */
static void crow_brows(uint8_t *fb, int dy, int slant, int lift)
{
	for (int e = 0; e < 2; e++) {
		int cx = e == 0 ? CROW_L : CROW_R;
		int s = e == 0 ? 1 : -1;
		int yo = CROW_Y - 17 + dy - lift;

		thick_line(fb, cx - 12 * s, yo - slant, cx + 11 * s, yo + slant, 3);
	}
}

static void crow_eye(uint8_t *fb, int cx, int cy, int r, bool highlight)
{
	ellipse(fb, cx, cy, r, r, true);
	if (highlight) {
		ellipse(fb, cx - 4, cy - 4, 2, 2, false);
	}
}

static void draw_crow(uint8_t *fb, enum mood mood, int64_t t_ms, bool dollars)
{
	if (dollars && (mood == MOOD_CHILL || mood == MOOD_BORED || mood == MOOD_WORKING || mood == MOOD_EXCITED)) {
		for (int e = 0; e < 2; e++) { /* shiny coins instead of eyes */
			int cx = e == 0 ? CROW_L : CROW_R;

			ring(fb, cx, CROW_Y, 13, 2);
			dollar(fb, cx, CROW_Y);
		}
		return;
	}
	for (int e = 0; e < 2; e++) {
		int cx = e == 0 ? CROW_L : CROW_R;
		int s = e == 0 ? 1 : -1;

		switch (mood) {
		case MOOD_CHILL:
			if (blinking(t_ms)) {
				fill_rect(fb, cx - 10, CROW_Y, 21, 2);
			} else {
				crow_eye(fb, cx, CROW_Y, CROW_EYE_R, true);
			}
			break;
		case MOOD_BORED: { /* heavy lids, drifting gaze */
			int dx = tri(t_ms, 6000, 16) - 8;

			crow_eye(fb, cx + dx, CROW_Y, CROW_EYE_R, true);
			clear_rect(fb, cx + dx - 12, CROW_Y - 12, 25, 11);
			fill_rect(fb, cx + dx - 11, CROW_Y - 2, 23, 2);
			break;
		}
		case MOOD_WORKING: /* pupils sweep inside ring eyes */
			ring(fb, cx, CROW_Y, CROW_EYE_R + 1, 2);
			ellipse(fb, cx + tri(t_ms, 1600, 10) - 5, CROW_Y + 1, 5, 5, true);
			break;
		case MOOD_SLEEPING:
			arc(fb, cx, CROW_Y - 4 + tri(t_ms, 4000, 2), 10, 7, 4, 28, 3);
			break;
		case MOOD_EXCITED: { /* crows like shiny things: twinkling sparkles */
			bool big = ((t_ms / 150) & 1) != 0;
			int dy = big ? -2 : 1;
			int a = big ? 17 : 12;
			int b = big ? 12 : 8;

			thick_line(fb, cx, CROW_Y - a + dy, cx, CROW_Y + a + dy, 3);
			thick_line(fb, cx - b, CROW_Y + dy, cx + b, CROW_Y + dy, 3);
			ellipse(fb, cx, CROW_Y + dy, 3, 3, true);
			if (big) {
				fill_rect(fb, cx + 14, CROW_Y - 13 + dy, 2, 2);
				fill_rect(fb, cx - 15, CROW_Y + 11 + dy, 2, 2);
			}
			break;
		}
		case MOOD_ALERT:
			ring(fb, cx, CROW_Y, 12, 2);
			ellipse(fb, cx, CROW_Y, 5, 5, true);
			break;
		case MOOD_CONFUSED: { /* head tilt: one eye higher, one brow cocked */
			int w = (int)((t_ms / 200) & 1);

			if (e == 0) {
				crow_eye(fb, cx + w, CROW_Y - 5, CROW_EYE_R, true);
				thick_line(fb, cx - 12, CROW_Y - 24, cx + 10, CROW_Y - 27, 3);
			} else {
				crow_eye(fb, cx - w, CROW_Y + 4, 7, true);
				thick_line(fb, cx - 9, CROW_Y - 8, cx + 10, CROW_Y - 9, 3);
			}
			break;
		}
		case MOOD_ANGRY:
			crow_eye(fb, cx, CROW_Y + 2, CROW_EYE_R, false);
			for (int i = -12; i <= 12; i++) {
				int top = CROW_Y - 9 + (i * s + 12) * 9 / 24;

				clear_rect(fb, cx + i, CROW_Y - 12, 1, top - (CROW_Y - 12));
			}
			thick_line(fb, cx - 13 * s, CROW_Y - 14, cx + 12 * s, CROW_Y - 4, 3);
			break;
		case MOOD_DIZZY:
			spiral(fb, cx + tri(t_ms, 1200, 8) - 4, CROW_Y, (int)((t_ms / 60) & 31));
			break;
		case MOOD_SCARED: {
			int dx = ((t_ms / 50) & 1) ? 1 : -1;

			ring(fb, cx + dx, CROW_Y, 12, 2);
			fill_rect(fb, cx + dx - 1, CROW_Y - 1, 3, 3);
			break;
		}
		case MOOD_LIMIT:
			break;
		}
	}
	switch (mood) {
	case MOOD_CHILL:
		crow_brows(fb, 0, 2, 0);
		break;
	case MOOD_BORED:
		crow_brows(fb, 4, 0, 0);
		break;
	case MOOD_WORKING:
		crow_brows(fb, 0, 4, 0);
		break;
	case MOOD_SLEEPING:
		zee(fb, 108, 12 - 2 * tri(t_ms, 4000, 2), 2);
		break;
	case MOOD_ALERT:
		crow_brows(fb, -2, -2, 3);
		if ((t_ms / 400) & 1) {
			invert_fb(fb);
		}
		break;
	case MOOD_SCARED:
		crow_brows(fb, -3, -3, 3);
		break;
	case MOOD_LIMIT:
		draw_limit(fb, t_ms);
		break;
	default:
		break;
	}
}

/* Beaver: big filled cartoon eyes with a highlight, low on the screen and close together. */
#define BEAVER_L 49
#define BEAVER_R 79
#define BEAVER_Y 40
#define BEAVER_RX 14
#define BEAVER_RY 17

static int half_floor(int v)
{
	return v >= 0 ? v / 2 : -((1 - v) / 2);
}

/* One eye sized from rx/ry, nudged by half of (px, py), with a 4x4 highlight cut out of its upper left. */
static void beaver_eye(uint8_t *fb, int cx, int cy, int rx, int ry, int px, int py)
{
	int ex = cx + half_floor(px);
	int ey = cy + half_floor(py);
	int a = rx - 3;
	int b = ry - 3;

	ellipse(fb, ex, ey, a, b, true);
	clear_rect(fb, ex - a / 2 - 1, ey - b / 2 - 2, 4, 4);
}

static void beaver_angry(uint8_t *fb, int cx, int s)
{
	beaver_eye(fb, cx, BEAVER_Y + 2, BEAVER_RX, BEAVER_RY - 2, 2 * s, 3);
	for (int i = -BEAVER_RX - 1; i <= BEAVER_RX + 1; i++) {
		int cut = (i * s + BEAVER_RX + 1) * 10 / (2 * BEAVER_RX + 2);

		clear_rect(fb, cx + i, BEAVER_Y - BEAVER_RY, 1, cut + 2);
	}
	thick_line(fb, cx - (BEAVER_RX + 2) * s, BEAVER_Y - BEAVER_RY, cx + (BEAVER_RX + 1) * s,
		   BEAVER_Y - BEAVER_RY + 10, 3);
}

static void draw_beaver(uint8_t *fb, enum mood mood, int64_t t_ms, bool dollars)
{
	if (dollars && (mood == MOOD_CHILL || mood == MOOD_BORED || mood == MOOD_WORKING || mood == MOOD_EXCITED)) {
		for (int e = 0; e < 2; e++) { /* dollar eyes */
			int cx = e == 0 ? BEAVER_L : BEAVER_R;

			dollar(fb, cx, BEAVER_Y);
		}
		return;
	}
	for (int e = 0; e < 2; e++) {
		int cx = e == 0 ? BEAVER_L : BEAVER_R;
		int s = e == 0 ? 1 : -1; /* pupils lean towards the middle */

		switch (mood) {
		case MOOD_CHILL:
			if (blinking(t_ms)) {
				fill_rect(fb, cx - BEAVER_RX, BEAVER_Y + 2, 2 * BEAVER_RX + 1, 3);
			} else {
				beaver_eye(fb, cx, BEAVER_Y, BEAVER_RX, BEAVER_RY, 2 * s, 3);
			}
			break;
		case MOOD_BORED:
			beaver_eye(fb, cx, BEAVER_Y, BEAVER_RX, BEAVER_RY, tri(t_ms, 6000, 8) - 4, 6);
			clear_rect(fb, cx - BEAVER_RX - 1, BEAVER_Y - BEAVER_RY - 1, 2 * BEAVER_RX + 3, BEAVER_RY + 1);
			fill_rect(fb, cx - BEAVER_RX, BEAVER_Y - 1, 2 * BEAVER_RX + 1, 3);
			break;
		case MOOD_WORKING: { /* busy beaver: determined squint, gnawing bob */
			int bob = (int)((t_ms / 140) & 1);

			beaver_eye(fb, cx, BEAVER_Y + 4 + bob, BEAVER_RX, 9, 3 * s, 1);
			thick_line(fb, cx - 13 * s, BEAVER_Y - 12, cx + 9 * s, BEAVER_Y - 6, 3);
			break;
		}
		case MOOD_SLEEPING:
			arc(fb, cx, BEAVER_Y + tri(t_ms, 4000, 2), BEAVER_RX - 1, 7, 3, 29, 3);
			break;
		case MOOD_EXCITED: /* happy ^ ^ */
			arc(fb, cx, BEAVER_Y + 8 + (((t_ms / 120) & 1) ? 3 : -2), BEAVER_RX, BEAVER_RY - 2, 35, 61, 4);
			break;
		case MOOD_ALERT: /* needs you: angry, and the screen flashes */
		case MOOD_ANGRY:
			beaver_angry(fb, cx, s);
			break;
		case MOOD_CONFUSED: {
			int w = (int)((t_ms / 200) & 1);

			if (e == 0) {
				beaver_eye(fb, cx + w, BEAVER_Y - 3, BEAVER_RX + 1, BEAVER_RY + 3, -3, -4);
			} else {
				beaver_eye(fb, cx - w, BEAVER_Y + 4, 8, 9, 2, 2);
			}
			break;
		}
		case MOOD_DIZZY:
			spiral(fb, cx + tri(t_ms, 1200, 6) - 3, BEAVER_Y, (int)((t_ms / 60) & 31));
			break;
		case MOOD_SCARED: {
			int dx = ((t_ms / 50) & 1) ? 1 : -1;

			ellipse(fb, cx + dx, BEAVER_Y - 4, 4, 5, true);
			clear_rect(fb, cx + dx - 2, BEAVER_Y - 7, 2, 2);
			break;
		}
		case MOOD_LIMIT:
			break;
		}
	}
	switch (mood) {
	case MOOD_WORKING: /* wood chips flying out of the bottom corners, just above the printed teeth */
		for (int k = 0; k < 4; k++) {
			int ph = (int)((t_ms / 90 + k * 5) % 20);
			int s = (k & 1) ? 1 : -1;

			fill_rect(fb, 64 + s * (22 + ph * 2), 62 - ph * 2 + ph * ph / 8, 2, 2);
		}
		break;
	case MOOD_SLEEPING:
		zee(fb, 96, 18 - 2 * tri(t_ms, 4000, 2), 2);
		zee(fb, 108, 8 - tri(t_ms, 4000, 2), 1);
		break;
	case MOOD_ALERT:
		if ((t_ms / 400) & 1) {
			invert_fb(fb);
		}
		break;
	case MOOD_LIMIT:
		draw_limit(fb, t_ms);
		break;
	default:
		break;
	}
}

static uint32_t face_still_change_ms(enum mood mood, int64_t t_ms, bool dollars)
{
	switch (mood) {
	case MOOD_CHILL: {
		if (dollars) {
			return FACE_STILL_MS; /* dollar eyes do not blink */
		}
		int phase = (int)(t_ms % BLINK_PERIOD_MS);

		return (uint32_t)(phase < BLINK_LEN_MS ? BLINK_LEN_MS - phase : BLINK_PERIOD_MS - phase);
	}
	case MOOD_ANGRY:
		return FACE_STILL_MS;
	case MOOD_LIMIT: {
		int phase = (int)(t_ms % 1000); /* the "!" is shown for the first 600 ms of every second */

		return (uint32_t)(phase < 600 ? 600 - phase : 1000 - phase);
	}
	case MOOD_SLEEPING: {
		/* Breathing: the eyes move down one pixel at these points of the 4 s cycle (see tri()). */
		static const int steps[] = {1000, 2000, 2001, 3000, 4000};
		int phase = (int)(t_ms % 4000);

		for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
			if (phase < steps[i]) {
				return (uint32_t)(steps[i] - phase);
			}
		}
		return 1;
	}
	default:
		return FACE_ANIMATED_MS;
	}
}

uint32_t face_next_change_ms(enum mood mood, int64_t t_ms, bool dollars, enum battery_level battery)
{
	uint32_t next = face_still_change_ms(mood, t_ms, dollars);

	if (battery >= BATTERY_LOW && next > until_low_icon_toggle(t_ms)) {
		next = until_low_icon_toggle(t_ms);
	}
	return next;
}

void face_render(uint8_t *fb, enum mood mood, int64_t t_ms, bool connected,
		 const struct mood_summary *sessions, uint8_t profile,
		 const struct buddy_usage *usage, enum battery_level battery)
{
	bool dollars = (usage->flags & BUDDY_USAGE_FLAG_OVER_WARNING) != 0;

	style = profile;
	memset(fb, 0, FACE_FB_SIZE);

	/*
	 * No bridge: the dead face, except once the buddy is bored or asleep. Nothing can happen then without a bridge, and
	 * a central that connects for a moment (a PC reconnecting to a paired buddy, a bridge that is refused) would
	 * otherwise flip the face between dead and bored.
	 */
	if (!connected && mood != MOOD_BORED && mood != MOOD_SLEEPING) {
		if (style == PROFILE_CROW || style == PROFILE_BEAVER) { /* crossed-out eyes, no mouth */
			int y = style == PROFILE_CROW ? CROW_Y : BEAVER_Y;

			cross(fb, style == PROFILE_CROW ? CROW_L : BEAVER_L, y);
			cross(fb, style == PROFILE_CROW ? CROW_R : BEAVER_R, y);
		} else {
			draw_dead_face(fb);
		}
		draw_sessions(fb, sessions, t_ms);
		if (battery >= BATTERY_LOW && low_icon_on(t_ms)) {
			draw_battery_icon(fb, FACE_WIDTH - 24, 2, 14, 7,
					  battery == BATTERY_CRITICAL ? 0 : 20);
		}
		return;
	}

	if (style == PROFILE_CROW || style == PROFILE_BEAVER) {
		if (style == PROFILE_CROW) {
			draw_crow(fb, mood, t_ms, dollars);
		} else {
			draw_beaver(fb, mood, t_ms, dollars);
		}
		draw_sessions(fb, sessions, t_ms);
		if (battery >= BATTERY_LOW && low_icon_on(t_ms)) {
			draw_battery_icon(fb, FACE_WIDTH - 24, 2, 14, 7, battery == BATTERY_CRITICAL ? 0 : 20);
		}
		return;
	}

	switch (mood) {
	case MOOD_SLEEPING:
		/* Closed eyes, slow breathing. */
		draw_eyes_open(fb, 0, tri(t_ms, 4000, 2), 3, 1000);
		break;
	case MOOD_CHILL:
		if (dollars) {
			draw_dollar_eyes(fb, 0, 0);
		} else {
			draw_eyes_open(fb, 0, 0, EYE_H, t_ms);
		}
		break;
	case MOOD_BORED:
		/* Heavy lids and a drifting gaze. */
		if (dollars) {
			draw_dollar_eyes(fb, tri(t_ms, 6000, 16) - 8, 4);
		} else {
			draw_eyes_open(fb, tri(t_ms, 6000, 16) - 8, 4, 12, t_ms * 3);
		}
		break;
	case MOOD_WORKING:
		/* Eyes sweep side to side. */
		if (dollars) {
			draw_dollar_eyes(fb, tri(t_ms, 1600, 20) - 10, 0);
		} else {
			draw_eyes_open(fb, tri(t_ms, 1600, 20) - 10, 0, 20, t_ms);
		}
		break;
	case MOOD_EXCITED: {
		int dy = ((t_ms / 120) & 1) ? 3 : -2;

		if (dollars) {
			draw_dollar_eyes(fb, 0, dy);
			break;
		}
		chevron(fb, EYE_L_X, EYE_Y + dy, true);
		chevron(fb, EYE_R_X, EYE_Y + dy, false);
		break;
	}
	case MOOD_ALERT:
		chevron(fb, EYE_L_X, EYE_Y, true);
		chevron(fb, EYE_R_X, EYE_Y, false);
		if ((t_ms / 400) & 1) {
			for (int i = 0; i < FACE_FB_SIZE; i++) {
				fb[i] = (uint8_t)~fb[i];
			}
		}
		break;
	case MOOD_CONFUSED: {
		int wob = (t_ms / 200) & 1;

		eye(fb, EYE_L_X + wob, EYE_Y - 2, EYE_W, 26);
		eye(fb, EYE_R_X - wob, EYE_Y + 6, EYE_W, 10);
		break;
	}
	case MOOD_ANGRY:
		draw_angry(fb);
		break;
	case MOOD_LIMIT:
		draw_limit(fb, t_ms);
		break;
	case MOOD_DIZZY: {
		int sway = tri(t_ms, 1200, 8) - 4;
		int phase = (int)((t_ms / 60) & 31);

		spiral(fb, EYE_L_X + sway, EYE_Y, phase);
		spiral(fb, EYE_R_X + sway, EYE_Y, phase);
		break;
	}
	case MOOD_SCARED:
		draw_scared(fb, t_ms);
		break;
	}

	draw_mouth(fb, mood, dollars, t_ms);
	draw_sessions(fb, sessions, t_ms);
	if (battery >= BATTERY_LOW && low_icon_on(t_ms)) {
		draw_battery_icon(fb, FACE_WIDTH - 24, 2, 14, 7, battery == BATTERY_CRITICAL ? 0 : 20);
	}
}
