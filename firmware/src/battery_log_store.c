#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>

#include "battery_log_store.h"

LOG_MODULE_REGISTER(battery_log_store, LOG_LEVEL_INF);

static struct battery_log_ring ring;
static struct k_spinlock lock;

/* Each record is its own setting, "battery_log/<slot>", so an hour costs one small flash write. */
static int log_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t wire[BATTERY_LOG_RECORD_SIZE];
	struct battery_log_record rec;

	if (len != sizeof(wire) || read_cb(cb_arg, wire, sizeof(wire)) != sizeof(wire)) {
		return -EINVAL;
	}
	if (battery_log_record_from_wire(&rec, wire, sizeof(wire)) != 0 || battery_log_slot_of(rec.seq) != strtoul(name, NULL, 10)) {
		LOG_WRN("Ignoring an invalid saved log record (%s)", name);
		return 0;
	}
	K_SPINLOCK(&lock) {
		(void)battery_log_ring_restore(&ring, &rec);
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(battery_log_handler, "battery_log", NULL, log_set, NULL, NULL);

int battery_log_store_init(void)
{
	int err;

	battery_log_ring_init(&ring);
	err = settings_load_subtree("battery_log");
	if (err != 0) {
		LOG_ERR("Loading the battery log failed (%d)", err);
		return err;
	}
	LOG_INF("Battery log: %u records, newest %u", battery_log_ring_count(&ring), (unsigned)ring.newest);
	return 0;
}

void battery_log_store_add(struct battery_log_record *rec)
{
	uint8_t wire[BATTERY_LOG_RECORD_SIZE];
	char key[24];
	unsigned slot;
	int err;

	K_SPINLOCK(&lock) {
		slot = battery_log_ring_add(&ring, rec);
	}
	battery_log_record_to_wire(rec, wire);
	snprintf(key, sizeof(key), "battery_log/%u", slot);
	err = settings_save_one(key, wire, sizeof(wire));
	if (err != 0) {
		LOG_ERR("Saving the battery log record failed (%d)", err);
	}
	LOG_INF("Battery log %u: %u %%, %u mV, %d uA, display %u s", (unsigned)rec->seq, rec->percent, rec->mv,
		rec->avg_current_10ua * 10, rec->lit_s);
}

size_t battery_log_store_read(uint32_t selected, uint8_t out[BATTERY_LOG_READ_MAX])
{
	size_t len;

	K_SPINLOCK(&lock) {
		len = battery_log_read_wire(&ring, selected, out);
	}
	return len;
}
