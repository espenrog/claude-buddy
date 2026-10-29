#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>

#include "battery.h"
#include "ble_svc.h"
#include "buzzer.h"
#include "cfg_store.h"
#include "face.h"
#include "measure_mode.h"
#include "mood.h"
#include "motion.h"
#include "ui.h"

LOG_MODULE_REGISTER(ui, LOG_LEVEL_INF);

#define MSG_QUEUE_DEPTH 16
#define WAKE_ON_POKE_MS 10000
#define PAGE_MS 15000 /* an extra screen goes back to the face after this long */
#define TOAST_MS 1500
#define PAIRING_SHOWN_MS 120000 /* as long as the pairing window in ble_svc.c */
#define MIN_SLEEP_MS 20
#define MAX_SLEEP_MS 1000 /* wake at least this often to re-check the mood and the clock-driven changes */

enum ui_msg_type {
	UI_MSG_EVENT,
	UI_MSG_POKE,
	UI_MSG_LINK,
	UI_MSG_USAGE,
	UI_MSG_PAGE,
	UI_MSG_BATTERY,
	UI_MSG_TOAST,
	UI_MSG_MEASURE,
	UI_MSG_PAIRING,
	UI_MSG_MOTION,
};

/* The page button steps through the extra screens and back to the face. */
enum ui_page {
	PAGE_FACE,
	PAGE_STATS,
	PAGE_BATTERY,
	PAGE_COUNT,
};

struct ui_msg {
	uint8_t type;
	uint8_t value;
	uint8_t slot;
};

K_MSGQ_DEFINE(ui_msgq, sizeof(struct ui_msg), MSG_QUEUE_DEPTH, 1);

#if DT_HAS_CHOSEN(zephyr_display)
static const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
#else
static const struct device *display;
#endif

static struct k_work_delayable frame_work;
static struct mood_ctx mood_ctx;
static uint8_t fb[FACE_FB_SIZE];
static uint8_t shown_fb[FACE_FB_SIZE]; /* what the display holds right now */
static bool shown_valid;
static int64_t next_nudge_ms; /* when to remind about a waiting session again; 0 = not waiting */
static unsigned nudges_done;
static bool connected;
static struct buddy_usage usage;
static struct buddy_usage usage_pending; /* written by the Bluetooth thread, under usage_lock */
static struct k_spinlock usage_lock;
static enum ui_page page;
static int64_t page_until_ms; /* an extra screen is showing until then */
static struct battery_state battery;
static struct battery_state battery_pending; /* written by the Bluetooth thread, under battery_lock */
static struct k_spinlock battery_lock;
static enum battery_level battery_level;
static const char *toast_text; /* a short message over whatever is showing, until toast_until_ms */
static int64_t toast_until_ms;
static const char *pairing_text; /* toast_text while the pairing window is shown */
static enum mood last_mood = MOOD_CHILL;
static int64_t wake_until_ms; /* a poke wakes a dark display for a while */
static bool blanked;
static int applied_contrast = -1;
static enum measure_mode measure; /* a held measurement mode, or MEASURE_NORMAL */
static int64_t measure_until_ms;
static atomic_t measure_published; /* for the Bluetooth thread: mode in the low byte, minutes left above it */

/* Switches the display off, or on at the given contrast; only talks to it when something changed. */
static void apply_display_state(bool blank, uint8_t contrast)
{
	if (display == NULL) {
		return;
	}
	if (blank != blanked) {
		int err = blank ? display_blanking_on(display) : display_blanking_off(display);

		if (err != 0 && err != -ENOSYS) {
			LOG_ERR("display blanking failed (%d)", err);
		}
		blanked = blank;
		shown_valid = false; /* do not trust the display contents across a blank */
	}
	if (!blank && contrast != applied_contrast) {
		int err = display_set_contrast(display, contrast);

		if (err != 0 && err != -ENOSYS) {
			LOG_ERR("display_set_contrast failed (%d)", err);
		}
		applied_contrast = contrast;
	}
}

static void post(uint8_t type, uint8_t value, uint8_t slot)
{
	struct ui_msg msg = {.type = type, .value = value, .slot = slot};

	if (k_msgq_put(&ui_msgq, &msg, K_NO_WAIT) != 0) {
		LOG_WRN("UI queue full, dropped message %u", type);
		return;
	}
	k_work_reschedule(&frame_work, K_NO_WAIT);
}

static void drain_messages(int64_t now)
{
	struct ui_msg msg;

	while (k_msgq_get(&ui_msgq, &msg, K_NO_WAIT) == 0) {
		switch (msg.type) {
		case UI_MSG_EVENT:
			if (mood_on_event(&mood_ctx, msg.value, msg.slot, now) != 0) {
				LOG_WRN("Unknown event 0x%02x (slot %u)", msg.value, msg.slot);
			} else {
				LOG_INF("Event 0x%02x (slot %u)", msg.value, msg.slot);
			}
			break;
		case UI_MSG_POKE:
			mood_on_poke(&mood_ctx, now);
			wake_until_ms = now + WAKE_ON_POKE_MS;
			break;
		case UI_MSG_MOTION:
			if (msg.value == MOTION_DOUBLE_TAP) {
				mood_on_poke(&mood_ctx, now);
			} else if (msg.value == MOTION_SHAKE) {
				mood_on_shake(&mood_ctx, now);
			} else if (msg.value == MOTION_FALL) {
				mood_on_fall(&mood_ctx, now);
			}
			wake_until_ms = MAX(wake_until_ms, now + WAKE_ON_POKE_MS); /* each of them lights a dark display */
			break;
		case UI_MSG_LINK:
			connected = msg.value != 0;
			break;
		case UI_MSG_USAGE:
			K_SPINLOCK(&usage_lock) {
				usage = usage_pending;
			}
			break;
		case UI_MSG_BATTERY:
			K_SPINLOCK(&battery_lock) {
				battery = battery_pending;
			}
			break;
		case UI_MSG_PAGE:
			page = (enum ui_page)((page + 1) % PAGE_COUNT);
			page_until_ms = now + PAGE_MS;
			wake_until_ms = now + PAGE_MS; /* a dark display must light up for it */
			break;
		case UI_MSG_MEASURE:
			if (msg.value == MEASURE_BEEP) {
				buzzer_play_always(SOUND_NEEDS_YOU); /* the loudest pattern, for the peak current */
				break;
			}
			measure = (enum measure_mode)msg.value;
			measure_until_ms = now + MEASURE_HOLD_MS;
			shown_valid = false;
			if (measure == MEASURE_NORMAL) {
				page = PAGE_FACE;
			}
			LOG_INF("Measurement mode %d", measure);
			break;
		case UI_MSG_PAIRING:
			if (msg.value == UI_PAIRING_CLOSED) {
				if (toast_text == pairing_text) {
					toast_until_ms = now;
				}
				break;
			}
			toast_text = msg.value == UI_PAIRING_OPEN ? (pairing_text = "PAIRING") : "PAIRED";
			toast_until_ms = now + (msg.value == UI_PAIRING_OPEN ? PAIRING_SHOWN_MS : 3 * TOAST_MS);
			wake_until_ms = MAX(wake_until_ms, toast_until_ms);
			shown_valid = false;
			break;
		case UI_MSG_TOAST:
			toast_text = msg.value != 0 ? "MUTED" : "UNMUTED";
			toast_until_ms = now + TOAST_MS;
			if (wake_until_ms < toast_until_ms) {
				wake_until_ms = toast_until_ms;
			}
			break;
		}
	}
}

static uint32_t next_wakeup_ms(int64_t now, enum mood mood, bool blank, bool page_showing);

static void frame_handler(struct k_work *work)
{
	int64_t now = k_uptime_get();
	enum mood mood;

	struct mood_summary sessions;

	drain_messages(now);
	if (measure != MEASURE_NORMAL && now >= measure_until_ms) {
		LOG_INF("Measurement mode %d ended", measure);
		measure = MEASURE_NORMAL;
		page = PAGE_FACE;
		shown_valid = false;
	}

	struct measure_look look;
	bool forced = measure_mode_look(measure, &look);

	mood_tick(&mood_ctx, now);
	mood = mood_get(&mood_ctx, now);
	mood_summarize(&mood_ctx, &sessions);
	if (forced) { /* a held mode ignores the sessions, and makes no sound of its own accord */
		mood = look.mood;
		memset(&sessions, 0, sizeof(sessions));
		page = (enum ui_page)look.page;
		page_until_ms = measure_until_ms;
	}
	atomic_set(&measure_published, (atomic_val_t)measure | ((atomic_val_t)((forced ? measure_until_ms - now : 0) / 1000) << 8));
	if (mood != last_mood) {
		LOG_INF("Mood %d -> %d", last_mood, mood);
		int sound = forced ? -1 : sound_for_transition(last_mood, mood);

		if (sound >= 0) {
			buzzer_play((enum sound)sound);
		}
		nudges_done = 0;
		next_nudge_ms = (!forced && mood == MOOD_ALERT) ? now + sound_nudge_delay_ms(0) : 0;
		last_mood = mood;
	}
	if (page != PAGE_FACE && now >= page_until_ms) {
		page = PAGE_FACE;
	}
	enum battery_level level = battery_level_next(battery_level, &battery);

	if (level != battery_level) {
		int sound = sound_for_battery(battery_level, level);

		LOG_INF("Battery level %d -> %d (%u %%)", battery_level, level, battery.percent);
		if (sound >= 0 && !forced) {
			buzzer_play((enum sound)sound);
		}
		battery_level = level;
	}
	if (!forced && next_nudge_ms != 0 && now >= next_nudge_ms) {
		buzzer_play(SOUND_NUDGE);
		nudges_done++;
		next_nudge_ms = sound_nudge_delay_ms(nudges_done) ? now + sound_nudge_delay_ms(nudges_done) : 0;
	}

	struct buddy_cfg cfg;
	uint8_t contrast;
	bool blank;
	uint8_t status[BUDDY_STATUS_SIZE];

	cfg_store_get(&cfg);
	contrast = (mood == MOOD_SLEEPING && now >= wake_until_ms) ? cfg.sleep_brightness
								  : cfg.brightness;
	if (forced) {
		contrast = look.blank ? 0 : (look.all_pixels ? 255 : cfg.brightness);
	}
	blank = (contrast == 0);
	apply_display_state(blank, contrast);

	bool page_showing = page != PAGE_FACE && mood != MOOD_ALERT && mood != MOOD_LIMIT;

	if (display != NULL && !blank) {
		struct display_buffer_descriptor desc = {
			.buf_size = sizeof(fb),
			.width = FACE_WIDTH,
			.height = FACE_HEIGHT,
			.pitch = FACE_WIDTH,
		};
		int err;

		/* Alerts and the limit always win: they are what you must not miss. */
		if (forced && look.all_pixels) {
			memset(fb, 0xFF, sizeof(fb));
		} else if (page_showing && page == PAGE_STATS) {
			face_render_stats(fb, &usage, connected);
		} else if (page_showing && page == PAGE_BATTERY) {
			face_render_battery(fb, &battery, battery_level, connected, now);
		} else {
			face_render(fb, mood, now, connected, &sessions, cfg.face_profile, &usage, battery_level);
		}
		if (!forced && now < toast_until_ms) {
			face_overlay_text(fb, toast_text);
		}
		/* Most frames are identical to the last one: do not wake the display bus for those. */
		if (!shown_valid || memcmp(fb, shown_fb, sizeof(fb)) != 0) {
			err = display_write(display, 0, 0, &desc, fb);
			if (err != 0) {
				LOG_ERR("display_write failed (%d)", err);
				shown_valid = false;
			} else {
				memcpy(shown_fb, fb, sizeof(fb));
				shown_valid = true;
			}
		}
	}

	status[0] = (uint8_t)mood;
	status[1] = sessions.active;
	status[2] = sessions.working;
	status[3] = sessions.waiting;
	status[4] = (blank ? BUDDY_STATUS_FLAG_DISPLAY_OFF : 0) |
		    ((cfg.flags & BUDDY_CFG_FLAG_MUTED) ? BUDDY_STATUS_FLAG_MUTED : 0) |
		    (forced ? BUDDY_STATUS_FLAG_MEASURE : 0);
	ble_svc_update_status(status);
	uint8_t battery_wire[BATTERY_WIRE_SIZE_FULL];

	battery_to_wire(&battery, battery_wire);
	ble_svc_update_battery(battery_wire);

	k_work_reschedule(&frame_work, K_MSEC(next_wakeup_ms(now, mood, blank, page_showing)));
}

/* Sleep until something on screen (or the mood, or a reminder) can change; an event wakes us earlier. */
static uint32_t next_wakeup_ms(int64_t now, enum mood mood, bool blank, bool page_showing)
{
	uint32_t wait = MAX_SLEEP_MS;

	if (!blank) {
		bool dollars = (usage.flags & BUDDY_USAGE_FLAG_OVER_WARNING) != 0;

		if (!page_showing) {
			wait = face_next_change_ms(mood, now, dollars, battery_level);
		} else if (page == PAGE_BATTERY) {
			wait = face_battery_next_change_ms(battery_level, now);
		}
	}
	/* Moments that change what we show or how bright it is. */
	const int64_t deadlines[] = {page_until_ms, wake_until_ms, next_nudge_ms, toast_until_ms, measure != MEASURE_NORMAL ? measure_until_ms : 0};

	for (size_t i = 0; i < sizeof(deadlines) / sizeof(deadlines[0]); i++) {
		if (deadlines[i] > now && (uint64_t)(deadlines[i] - now) < wait) {
			wait = (uint32_t)(deadlines[i] - now);
		}
	}
	return wait < MIN_SLEEP_MS ? MIN_SLEEP_MS : (wait > MAX_SLEEP_MS ? MAX_SLEEP_MS : wait);
}

int ui_init(void)
{
	k_work_init_delayable(&frame_work, frame_handler);
	mood_init(&mood_ctx, k_uptime_get());

	if (display == NULL) {
		LOG_WRN("No display configured; running headless");
	} else if (!device_is_ready(display)) {
		LOG_ERR("Display device not ready; running headless");
		display = NULL;
	} else {
		int err = display_blanking_off(display);

		if (err != 0 && err != -ENOSYS) {
			LOG_ERR("display_blanking_off failed (%d)", err);
		}
	}

	k_work_reschedule(&frame_work, K_NO_WAIT);
	return 0;
}

void ui_post_event(uint8_t code, uint8_t slot)
{
	post(UI_MSG_EVENT, code, slot);
}

void ui_post_poke(void)
{
	post(UI_MSG_POKE, 0, 0);
}

void ui_post_motion(uint8_t event)
{
	post(UI_MSG_MOTION, event, 0);
}

void ui_set_usage(const struct buddy_usage *new_usage)
{
	/* Called from the Bluetooth thread; the value is too big for a message, so take it under a lock. */
	K_SPINLOCK(&usage_lock) {
		usage_pending = *new_usage;
	}
	post(UI_MSG_USAGE, 0, 0);
}

void ui_set_battery(const struct battery_state *new_battery)
{
	K_SPINLOCK(&battery_lock) {
		battery_pending = *new_battery;
	}
	post(UI_MSG_BATTERY, 0, 0);
}

void ui_set_measure(uint8_t mode)
{
	post(UI_MSG_MEASURE, mode, 0);
}

void ui_measure_wire(uint8_t out[MEASURE_READ_SIZE])
{
	atomic_val_t v = atomic_get(&measure_published);

	measure_mode_to_wire((enum measure_mode)(v & 0xFF), (uint32_t)(v >> 8) * 1000u, out);
}

bool ui_measure_active(void)
{
	return (atomic_get(&measure_published) & 0xFF) != MEASURE_NORMAL;
}

bool ui_display_lit(void)
{
	return display != NULL && !blanked;
}

void ui_show_mute_toast(bool muted)
{
	post(UI_MSG_TOAST, muted ? 1 : 0, 0);
}

void ui_show_pairing(enum ui_pairing state)
{
	post(UI_MSG_PAIRING, state, 0);
}

void ui_post_page_toggle(void)
{
	post(UI_MSG_PAGE, 0, 0);
}

void ui_set_connected(bool is_connected)
{
	post(UI_MSG_LINK, is_connected ? 1 : 0, 0);
}
