#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <dk_buttons_and_leds.h>

#include "ble_svc.h"
#include "battery_gauge.h"
#include "battery_log_store.h"
#include "buzzer.h"
#include "cfg_store.h"
#include "motion.h"
#include "ui.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/*
 * The XIAO's own user button pokes. The mute button is also the pairing button: a short press toggles mute (acted on at
 * release), holding it 3 s opens the pairing window, 8 s forgets every pairing. The page button flips the pages. The
 * eye style is set from the web page.
 */
#define POKE_BUTTON DK_BTN1_MSK
#define MUTE_BUTTON DK_BTN2_MSK
#define PAGE_BUTTON DK_BTN3_MSK

static int on_battery(const struct battery_state *battery)
{
	ui_set_battery(battery);
	return 0;
}

static void on_motion(enum motion_event event)
{
	ui_post_motion((uint8_t)event);
}

static void on_link(bool connected)
{
	ui_set_connected(connected);
}

#define PAIR_HOLD_MS 3000
#define FORGET_HOLD_MS 8000
static bool pair_held_long;

static void pair_hold_fn(struct k_work *work)
{
	pair_held_long = true;
	ble_svc_pairing_open();
}

static void forget_hold_fn(struct k_work *work)
{
	pair_held_long = true;
	ble_svc_forget_bonds();
}

static K_WORK_DELAYABLE_DEFINE(pair_hold_work, pair_hold_fn);
static K_WORK_DELAYABLE_DEFINE(forget_hold_work, forget_hold_fn);

static void toggle_mute(void)
{
	struct buddy_cfg cfg;

	cfg_store_toggle_mute();
	cfg_store_get(&cfg);
	ui_show_mute_toast((cfg.flags & BUDDY_CFG_FLAG_MUTED) != 0);
	buzzer_play(SOUND_UNMUTED); /* silent unless the buzzer was just unmuted */
}

static void on_button(uint32_t button_state, uint32_t has_changed)
{
	uint32_t pressed = button_state & has_changed;
	uint32_t released = ~button_state & has_changed;

	if (pressed & MUTE_BUTTON) {
		pair_held_long = false;
		k_work_reschedule(&pair_hold_work, K_MSEC(PAIR_HOLD_MS));
		k_work_reschedule(&forget_hold_work, K_MSEC(FORGET_HOLD_MS));
	}

	if (pressed & POKE_BUTTON) {
		ui_post_poke();
	}
	if ((released & MUTE_BUTTON) != 0) {
		(void)k_work_cancel_delayable(&pair_hold_work);
		(void)k_work_cancel_delayable(&forget_hold_work);
		if (!pair_held_long) {
			toggle_mute();
		}
	}
	if (pressed & PAGE_BUTTON) {
		ui_post_page_toggle();
	}
}

int main(void)
{
	int err;

	err = cfg_store_init();
	if (err != 0) {
		LOG_WRN("Continuing with default settings");
	}

	err = battery_log_store_init();
	if (err != 0) {
		LOG_WRN("Continuing without the saved battery log");
	}

	err = dk_buttons_init(on_button);
	if (err != 0) {
		LOG_ERR("Buttons init failed (%d)", err);
		return 0;
	}

	err = buzzer_init();
	if (err != 0) {
		LOG_WRN("Continuing without a buzzer");
	}

	err = battery_gauge_init();
	if (err != 0) {
		LOG_WRN("Continuing without a battery gauge");
	}

	err = ui_init();
	if (err != 0) {
		LOG_ERR("UI init failed (%d)", err);
		return 0;
	}

	err = motion_init(on_motion);
	if (err != 0 && err != -ENODEV) {
		LOG_WRN("Continuing without the accelerometer (%d)", err);
	}

	err = ble_svc_init(ui_post_event, on_link, ui_set_usage, buzzer_set_time, on_battery);
	if (err != 0) {
		return 0;
	}

	return 0; /* everything else happens in other threads and callbacks */
}
