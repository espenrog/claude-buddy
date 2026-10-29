#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>

#include "cfg_store.h"

LOG_MODULE_REGISTER(cfg_store, LOG_LEVEL_INF);

#define SAVE_DELAY_MS 2000 /* merges a burst of changes (a slider being dragged) into one write */

static struct buddy_cfg current;
static struct k_spinlock lock;
static struct k_work_delayable save_work;

static void save_handler(struct k_work *work)
{
	uint8_t wire[BUDDY_CFG_WIRE_SIZE];
	int err;

	cfg_store_get_wire(wire);
	err = settings_save_one("buddy/cfg", wire, sizeof(wire));
	if (err != 0) {
		LOG_ERR("Saving settings failed (%d)", err);
	}
}

static int cfg_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t wire[BUDDY_CFG_WIRE_SIZE];
	struct buddy_cfg loaded;

	if (strcmp(name, "cfg") != 0) {
		return -ENOENT;
	}
	if (len != sizeof(wire) || read_cb(cb_arg, wire, sizeof(wire)) != sizeof(wire)) {
		return -EINVAL;
	}
	if (buddy_cfg_from_wire(&loaded, wire, sizeof(wire)) == 0) {
		current = loaded;
		LOG_INF("Loaded saved settings");
	} else {
		LOG_WRN("Ignoring invalid saved settings");
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(buddy_cfg_handler, "buddy", NULL, cfg_set, NULL, NULL);

int cfg_store_init(void)
{
	int err;

	buddy_cfg_defaults(&current);
	k_work_init_delayable(&save_work, save_handler);

	err = settings_subsys_init();
	if (err != 0) {
		LOG_ERR("Settings init failed (%d): running with defaults, nothing will be saved", err);
		return err;
	}
	err = settings_load_subtree("buddy");
	if (err != 0) {
		LOG_ERR("Loading settings failed (%d)", err);
	}
	return err;
}

void cfg_store_get(struct buddy_cfg *out)
{
	K_SPINLOCK(&lock) {
		*out = current;
	}
}

void cfg_store_get_wire(uint8_t out[BUDDY_CFG_WIRE_SIZE])
{
	struct buddy_cfg copy;

	cfg_store_get(&copy);
	buddy_cfg_to_wire(&copy, out);
}

static void changed(void)
{
	k_work_reschedule(&save_work, K_MSEC(SAVE_DELAY_MS));
}

int cfg_store_set_wire(const uint8_t *buf, size_t len)
{
	struct buddy_cfg next;

	if (buddy_cfg_from_wire(&next, buf, len) != 0) {
		return -EINVAL;
	}
	K_SPINLOCK(&lock) {
		current = next;
	}
	changed();
	return 0;
}

void cfg_store_toggle_mute(void)
{
	K_SPINLOCK(&lock) {
		current.flags ^= BUDDY_CFG_FLAG_MUTED;
	}
	changed();
}
