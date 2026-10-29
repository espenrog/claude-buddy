/* The hourly battery log (battery_log.h) in RAM, saved to flash a record at a time. Safe to call from any thread. */

#ifndef BATTERY_LOG_STORE_H_
#define BATTERY_LOG_STORE_H_

#include <stddef.h>
#include <stdint.h>

#include "battery_log.h"

/* Loads the saved records. Call once at start-up, after cfg_store_init (which starts the settings). */
int battery_log_store_init(void);

/* Adds a record (numbering it) and schedules it to be saved. */
void battery_log_store_add(struct battery_log_record *rec);

/* The answer to a read of the log characteristic when `selected` was written last. Returns the length. */
size_t battery_log_store_read(uint32_t selected, uint8_t out[BATTERY_LOG_READ_MAX]);

#endif /* BATTERY_LOG_STORE_H_ */
