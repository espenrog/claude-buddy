/* Settings held in RAM, saved to flash a moment after they change. Safe to call from any thread. */

#ifndef CFG_STORE_H_
#define CFG_STORE_H_

#include <stddef.h>
#include <stdint.h>

#include "buddy_cfg.h"

/* Loads saved settings (defaults if none). Call once at start-up. */
int cfg_store_init(void);

void cfg_store_get(struct buddy_cfg *out);
void cfg_store_get_wire(uint8_t out[BUDDY_CFG_WIRE_SIZE]);

/* Validates, applies and schedules a save. Returns 0 or -EINVAL (nothing changes). */
int cfg_store_set_wire(const uint8_t *buf, size_t len);

void cfg_store_next_profile(void);
void cfg_store_toggle_mute(void);

#endif /* CFG_STORE_H_ */
