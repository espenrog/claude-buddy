/* BLE peripheral: advertising, connection tracking and the event characteristic. */

#ifndef BLE_SVC_H_
#define BLE_SVC_H_

#include <stdbool.h>
#include <stdint.h>

#include "battery.h"
#include "buddy_cfg.h"

/* Service UUID 6b1d0001-5c3a-4e5b-9b2a-6c1a0d0e0001, event ...0002, version ...0003, settings ...0004, status ...0005, usage ...0006, time ...0007, battery ...0008, battery log ...000a, measurement mode ...000b. */

/* Bump the protocol version when the GATT layout or event codes change incompatibly. */
#define BUDDY_PROTOCOL_VERSION 2
#define BUDDY_FW_VERSION_MAJOR 0
#define BUDDY_FW_VERSION_MINOR 8
#define BUDDY_FW_VERSION_PATCH 14

/* Capability bits, the fourth byte of the version characteristic. */
/* 0x01 is not used (it was a "restart into a firmware loader" command on an earlier board). */
#define BUDDY_CAP_BATTERY_SIM 0x02 /* the battery characteristic accepts simulated readings */
#define BUDDY_CAP_BATTERY_LOG 0x04 /* the battery log characteristic exists */
#define BUDDY_CAP_MEASURE 0x08     /* the measurement mode characteristic exists */
#define BUDDY_CAP_PAIRING 0x10     /* everything but the version needs a paired (encrypted) link */
#define BUDDY_CAP_SMP_UPDATE 0x20  /* the application takes firmware updates itself (SMP service) */

/* Size of the status characteristic: [mood, sessions, working, waiting, flags]. */
#define BUDDY_STATUS_SIZE 5
#define BUDDY_STATUS_FLAG_DISPLAY_OFF 0x01
#define BUDDY_STATUS_FLAG_MUTED 0x02
#define BUDDY_STATUS_FLAG_MEASURE 0x04 /* a measurement mode holds the buddy in one state */

/* Called from the Bluetooth thread: queue the work, do not block. */
typedef void (*ble_event_cb_t)(uint8_t code, uint8_t slot);
typedef void (*ble_link_cb_t)(bool connected);
typedef void (*ble_usage_cb_t)(const struct buddy_usage *usage);
typedef int (*ble_time_cb_t)(const uint8_t *wire, size_t len);
typedef int (*ble_battery_cb_t)(const struct battery_state *battery);

int ble_svc_init(ble_event_cb_t event_cb, ble_link_cb_t link_cb, ble_usage_cb_t usage_cb,
		 ble_time_cb_t time_cb, ble_battery_cb_t battery_cb);

/* Pairing window (no-ops when BUDDY_REQUIRE_PAIRING is off): new pairings are accepted only while it is open. */
void ble_svc_pairing_open(void);
/* Forgets every pairing, drops the link and opens the pairing window. */
void ble_svc_forget_bonds(void);

/* Publishes the current state; subscribed centrals are notified when it changes. */
void ble_svc_update_status(const uint8_t status[BUDDY_STATUS_SIZE]);

/* Publishes the battery state; subscribed centrals are notified when it changes. */
void ble_svc_update_battery(const uint8_t battery[BATTERY_WIRE_SIZE_FULL]);

#endif /* BLE_SVC_H_ */
