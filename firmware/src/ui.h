/* Owns the display and the mood state; events are queued from any thread. */

#ifndef UI_H_
#define UI_H_

#include <stdbool.h>
#include <stdint.h>

#include "battery.h"
#include "buddy_cfg.h"

int ui_init(void);

/* Queues a Claude event code for a session slot; safe from any thread. */
void ui_post_event(uint8_t code, uint8_t slot);

/* Queues a poke (button press). */
void ui_post_poke(void);

/* Queues something the accelerometer noticed (enum motion_event in motion.h). */
void ui_post_motion(uint8_t event);

void ui_set_connected(bool connected);

/* Spending summary from the bridge. */
void ui_set_usage(const struct buddy_usage *usage);

/* Battery reading from a fuel gauge, or from a test write over Bluetooth. */
void ui_set_battery(const struct battery_state *battery);

/* Measurement mode (measure_mode.h): hold the buddy in one state for a power measurement; MEASURE_BEEP plays a sound once. */
void ui_set_measure(uint8_t mode);

/* The measurement mode characteristic: [format, mode, minutes left]. Safe from any thread. */
void ui_measure_wire(uint8_t out[3]);

/* Whether a measurement mode is held right now; safe from any thread. */
bool ui_measure_active(void);

/* Whether the display is lit right now (not blanked); safe from any thread. */
bool ui_display_lit(void);

/* Shows "MUTED" or "UNMUTED" for a moment, over whatever is on screen. */
void ui_show_mute_toast(bool muted);

/* The pairing window shown on the display ("PAIRING" until it closes, "PAIRED" for a moment). */
enum ui_pairing { UI_PAIRING_CLOSED, UI_PAIRING_OPEN, UI_PAIRING_DONE };
void ui_show_pairing(enum ui_pairing state);

/* Button: step to the next extra screen (stats, battery) and back to the face. */
void ui_post_page_toggle(void);

#endif /* UI_H_ */
