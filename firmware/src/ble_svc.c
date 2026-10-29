#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>

#include "ble_svc.h"
#include "battery_log_store.h"
#include "buddy_cfg.h"
#include "cfg_store.h"
#include "measure_mode.h"
#include "mood.h"
#include "ui.h"
#ifdef CONFIG_BUDDY_SMP_UPDATE
#include "smp_update.h"
#endif

LOG_MODULE_REGISTER(ble_svc, LOG_LEVEL_INF);

/* 150 to 250 ms between connection events (units of 1.25 ms), the buddy may skip 1, drop the link after 4 s. */
#define CONN_INTERVAL_MIN 120
#define CONN_INTERVAL_MAX 200
#define CONN_LATENCY 1
#define CONN_TIMEOUT 400

#define CLAUDE_SVC_UUID_VAL \
	BT_UUID_128_ENCODE(0x6b1d0001, 0x5c3a, 0x4e5b, 0x9b2a, 0x6c1a0d0e0001)
#define CLAUDE_EVT_UUID_VAL \
	BT_UUID_128_ENCODE(0x6b1d0002, 0x5c3a, 0x4e5b, 0x9b2a, 0x6c1a0d0e0001)
#define CLAUDE_VER_UUID_VAL \
	BT_UUID_128_ENCODE(0x6b1d0003, 0x5c3a, 0x4e5b, 0x9b2a, 0x6c1a0d0e0001)
#define CLAUDE_CFG_UUID_VAL \
	BT_UUID_128_ENCODE(0x6b1d0004, 0x5c3a, 0x4e5b, 0x9b2a, 0x6c1a0d0e0001)
#define CLAUDE_STATUS_UUID_VAL \
	BT_UUID_128_ENCODE(0x6b1d0005, 0x5c3a, 0x4e5b, 0x9b2a, 0x6c1a0d0e0001)

#define CLAUDE_USAGE_UUID_VAL \
	BT_UUID_128_ENCODE(0x6b1d0006, 0x5c3a, 0x4e5b, 0x9b2a, 0x6c1a0d0e0001)

#define CLAUDE_TIME_UUID_VAL \
	BT_UUID_128_ENCODE(0x6b1d0007, 0x5c3a, 0x4e5b, 0x9b2a, 0x6c1a0d0e0001)

#define CLAUDE_BATTERY_UUID_VAL \
	BT_UUID_128_ENCODE(0x6b1d0008, 0x5c3a, 0x4e5b, 0x9b2a, 0x6c1a0d0e0001)

#define CLAUDE_LOG_UUID_VAL \
	BT_UUID_128_ENCODE(0x6b1d000a, 0x5c3a, 0x4e5b, 0x9b2a, 0x6c1a0d0e0001)

#define CLAUDE_MEASURE_UUID_VAL \
	BT_UUID_128_ENCODE(0x6b1d000b, 0x5c3a, 0x4e5b, 0x9b2a, 0x6c1a0d0e0001)

static const struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(CLAUDE_SVC_UUID_VAL);
static const struct bt_uuid_128 evt_uuid = BT_UUID_INIT_128(CLAUDE_EVT_UUID_VAL);
static const struct bt_uuid_128 ver_uuid = BT_UUID_INIT_128(CLAUDE_VER_UUID_VAL);
static const struct bt_uuid_128 cfg_uuid = BT_UUID_INIT_128(CLAUDE_CFG_UUID_VAL);
static const struct bt_uuid_128 status_uuid = BT_UUID_INIT_128(CLAUDE_STATUS_UUID_VAL);
static const struct bt_uuid_128 usage_uuid = BT_UUID_INIT_128(CLAUDE_USAGE_UUID_VAL);
static const struct bt_uuid_128 time_uuid = BT_UUID_INIT_128(CLAUDE_TIME_UUID_VAL);
static const struct bt_uuid_128 battery_uuid = BT_UUID_INIT_128(CLAUDE_BATTERY_UUID_VAL);
static const struct bt_uuid_128 log_uuid = BT_UUID_INIT_128(CLAUDE_LOG_UUID_VAL);
static const struct bt_uuid_128 measure_uuid = BT_UUID_INIT_128(CLAUDE_MEASURE_UUID_VAL);

static uint8_t status_val[BUDDY_STATUS_SIZE];
static struct k_spinlock status_lock;

/* [protocol version, firmware major, firmware minor, capability bits, firmware patch] */
static const uint8_t version_info[] = {
	BUDDY_PROTOCOL_VERSION, BUDDY_FW_VERSION_MAJOR, BUDDY_FW_VERSION_MINOR,
	(IS_ENABLED(CONFIG_BUDDY_BATTERY_SIMULATION) ? BUDDY_CAP_BATTERY_SIM : 0) | BUDDY_CAP_BATTERY_LOG | BUDDY_CAP_MEASURE |
		(IS_ENABLED(CONFIG_BUDDY_REQUIRE_PAIRING) ? BUDDY_CAP_PAIRING : 0) |
		(IS_ENABLED(CONFIG_BUDDY_SMP_UPDATE) ? BUDDY_CAP_SMP_UPDATE : 0),
	BUDDY_FW_VERSION_PATCH,
};

static ble_event_cb_t event_cb;
static ble_link_cb_t link_cb;
static ble_usage_cb_t usage_cb;
static ble_time_cb_t time_cb;
static ble_battery_cb_t battery_cb;
static uint8_t battery_val[BATTERY_WIRE_SIZE_FULL] = {BATTERY_FORMAT, 0, 0, 0, 0, 0x00, 0x80, 0x80, 0x80};
static struct k_spinlock battery_lock;
static uint32_t log_selected; /* the record the next read of the log characteristic returns (one central at a time) */
static struct k_work adv_work;

/* Characteristics need an encrypted link (a paired central), except the version, which tells a central what to expect. */
#ifdef CONFIG_BUDDY_REQUIRE_PAIRING
#define PERM_R BT_GATT_PERM_READ_ENCRYPT
#define PERM_W (BT_GATT_PERM_WRITE_ENCRYPT | BT_GATT_PERM_PREPARE_WRITE)
#define PERM_RW (PERM_R | PERM_W)
#define PERM_CCC (BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT)
#else
#define PERM_R BT_GATT_PERM_READ
#define PERM_W (BT_GATT_PERM_WRITE | BT_GATT_PERM_PREPARE_WRITE)
#define PERM_RW (PERM_R | PERM_W)
#define PERM_CCC (BT_GATT_PERM_READ | BT_GATT_PERM_WRITE)
#endif

/* Filled in by name_init(): "<CONFIG_BT_DEVICE_NAME> A1B2", the suffix being the last two bytes of the device address. */
static struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, NULL, 0),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, CLAUDE_SVC_UUID_VAL),
};

/*
 * Also reached for prepared (long/reliable) writes. The stack first calls this with
 * BT_GATT_WRITE_FLAG_PREPARE (authorization only, must return 0), queues the data, and
 * calls it again at the execute step with the real bytes and BT_GATT_WRITE_FLAG_EXECUTE.
 */
static ssize_t event_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			   uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	LOG_DBG("Event write: len %u offset %u flags 0x%02x", len, offset, flags);

	if ((flags & BT_GATT_WRITE_FLAG_PREPARE) != 0U) {
		/* Authorization check only; the data arrives again at the execute step. */
		return 0;
	}

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	/* Protocol 2: [event code, session slot]. Protocol 1 sent just [event code] (slot 0). */
	if (len != 1 && len != 2) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	const uint8_t *data = buf;
	uint8_t slot = (len == 2) ? data[1] : 0;

	if (slot >= MOOD_MAX_SESSIONS) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	if (event_cb != NULL) {
		event_cb(data[0], slot);
	}

	return len;
}

static ssize_t version_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			    uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, version_info, sizeof(version_info));
}

static ssize_t cfg_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			uint16_t len, uint16_t offset)
{
	uint8_t wire[BUDDY_CFG_WIRE_SIZE];

	cfg_store_get_wire(wire);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, wire, sizeof(wire));
}

static ssize_t cfg_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			 uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	if ((flags & BT_GATT_WRITE_FLAG_PREPARE) != 0U) {
		return 0; /* authorization only; the data arrives again at the execute step */
	}
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (cfg_store_set_wire(buf, len) != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	LOG_INF("Settings updated over Bluetooth");
	return len;
}

static ssize_t usage_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			   uint16_t len, uint16_t offset, uint8_t flags)
{
	struct buddy_usage usage;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	if ((flags & BT_GATT_WRITE_FLAG_PREPARE) != 0U) {
		return 0; /* authorization only; the data arrives again at the execute step */
	}
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (buddy_usage_from_wire(&usage, buf, len) != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	if (usage_cb != NULL) {
		usage_cb(&usage);
	}
	return len;
}

static ssize_t time_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			  uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	if ((flags & BT_GATT_WRITE_FLAG_PREPARE) != 0U) {
		return 0; /* authorization only; the data arrives again at the execute step */
	}
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (time_cb == NULL || time_cb(buf, len) != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	return len;
}

static ssize_t battery_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			    uint16_t len, uint16_t offset)
{
	uint8_t copy[BATTERY_WIRE_SIZE_FULL];

	K_SPINLOCK(&battery_lock) {
		memcpy(copy, battery_val, sizeof(copy));
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset, copy, sizeof(copy));
}

/* A test write, accepted only in builds without a real fuel gauge (CONFIG_BUDDY_BATTERY_SIMULATION). */
static ssize_t battery_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			     uint16_t len, uint16_t offset, uint8_t flags)
{
	struct battery_state state;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	if (!IS_ENABLED(CONFIG_BUDDY_BATTERY_SIMULATION)) {
		return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
	}
	if ((flags & BT_GATT_WRITE_FLAG_PREPARE) != 0U) {
		return 0; /* authorization only; the data arrives again at the execute step */
	}
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (battery_from_wire(&state, buf, len) != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	if (battery_cb != NULL) {
		battery_cb(&state);
	}
	return len;
}

static void battery_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	LOG_DBG("Battery notifications %s", value == BT_GATT_CCC_NOTIFY ? "on" : "off");
}

/* Write the sequence number (u32 le) of the record to read, then read: see docs/PROTOCOL.md. */
static ssize_t log_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			 uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *data = buf;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	if ((flags & BT_GATT_WRITE_FLAG_PREPARE) != 0U) {
		return 0; /* authorization only; the data arrives again at the execute step */
	}
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len != 4) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	log_selected = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
	return len;
}

static ssize_t log_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			uint16_t len, uint16_t offset)
{
	uint8_t copy[BATTERY_LOG_READ_MAX];
	size_t n = battery_log_store_read(log_selected, copy);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, copy, n);
}

/* [format 1, mode]: hold the buddy in one state for a power measurement (measure_mode.h). Read: [format, mode, minutes left]. */
static ssize_t measure_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			     uint16_t len, uint16_t offset, uint8_t flags)
{
	enum measure_mode mode;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	if ((flags & BT_GATT_WRITE_FLAG_PREPARE) != 0U) {
		return 0; /* authorization only; the data arrives again at the execute step */
	}
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (measure_mode_from_wire(&mode, buf, len) != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	ui_set_measure(mode);
	return len;
}

static ssize_t measure_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			    uint16_t len, uint16_t offset)
{
	uint8_t copy[MEASURE_READ_SIZE];

	ui_measure_wire(copy);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, copy, sizeof(copy));
}

static ssize_t status_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	uint8_t copy[BUDDY_STATUS_SIZE];

	K_SPINLOCK(&status_lock) {
		memcpy(copy, status_val, sizeof(copy));
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset, copy, sizeof(copy));
}

static void status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	LOG_DBG("Status notifications %s", value == BT_GATT_CCC_NOTIFY ? "on" : "off");
}

BT_GATT_SERVICE_DEFINE(claude_svc,
	BT_GATT_PRIMARY_SERVICE(&svc_uuid),
	BT_GATT_CHARACTERISTIC(&evt_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       PERM_W, NULL, event_write, NULL),
	BT_GATT_CHARACTERISTIC(&ver_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, /* open: it tells a central whether pairing is needed */
			       version_read, NULL, NULL),
	BT_GATT_CHARACTERISTIC(&cfg_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       PERM_RW,
			       cfg_read, cfg_write, NULL),
	BT_GATT_CHARACTERISTIC(&status_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       PERM_R, status_read, NULL, NULL),
	BT_GATT_CCC(status_ccc_changed, PERM_CCC),
	BT_GATT_CHARACTERISTIC(&usage_uuid.uuid, BT_GATT_CHRC_WRITE,
			       PERM_W, NULL, usage_write, NULL),
	BT_GATT_CHARACTERISTIC(&time_uuid.uuid, BT_GATT_CHRC_WRITE,
			       PERM_W, NULL, time_write, NULL),
	BT_GATT_CHARACTERISTIC(&battery_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY | BT_GATT_CHRC_WRITE,
			       PERM_RW,
			       battery_read, battery_write, NULL),
	BT_GATT_CCC(battery_ccc_changed, PERM_CCC),
	BT_GATT_CHARACTERISTIC(&log_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       PERM_RW,
			       log_read, log_write, NULL),
	BT_GATT_CHARACTERISTIC(&measure_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       PERM_RW,
			       measure_read, measure_write, NULL),
);

void ble_svc_update_battery(const uint8_t battery[BATTERY_WIRE_SIZE_FULL])
{
	bool changed = false;

	K_SPINLOCK(&battery_lock) {
		if (memcmp(battery_val, battery, sizeof(battery_val)) != 0) {
			memcpy(battery_val, battery, sizeof(battery_val));
			changed = true;
		}
	}
	if (changed) {
		(void)bt_gatt_notify_uuid(NULL, &battery_uuid.uuid, claude_svc.attrs, battery, BATTERY_WIRE_SIZE_FULL);
	}
}

void ble_svc_update_status(const uint8_t status[BUDDY_STATUS_SIZE])
{
	bool changed = false;

	K_SPINLOCK(&status_lock) {
		if (memcmp(status_val, status, sizeof(status_val)) != 0) {
			memcpy(status_val, status, sizeof(status_val));
			changed = true;
		}
	}
	if (changed) {
		/* -ENOTCONN when nobody is subscribed is normal */
		(void)bt_gatt_notify_uuid(NULL, &status_uuid.uuid, claude_svc.attrs, status,
					  BUDDY_STATUS_SIZE);
	}
}

/* Gives this buddy its own name so that several in range can be told apart. */
static void name_init(void)
{
	char name[CONFIG_BT_DEVICE_NAME_MAX + 1];
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);
	const char *shown;

	bt_id_get(addrs, &count);
	if (count > 0) {
		snprintf(name, sizeof(name), "%s %02X%02X", CONFIG_BT_DEVICE_NAME, addrs[0].a.val[1],
			 addrs[0].a.val[0]);
		if (bt_set_name(name) != 0) {
			LOG_WRN("Could not set the device name");
		}
	}
	shown = bt_get_name();
	ad[1].data = (const uint8_t *)shown;
	ad[1].data_len = strlen(shown);
	LOG_INF("Name: %s", shown);
}

static void adv_work_handler(struct k_work *work)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err != 0) {
		LOG_ERR("Advertising failed to start (%d)", err);
		return;
	}
	LOG_INF("Advertising");
}

#ifdef CONFIG_BUDDY_REQUIRE_PAIRING
#define PAIRING_WINDOW_S 120

static atomic_t pairing_is_open;
static struct k_work_delayable pairing_close_work;

static void pairing_close(struct k_work *work)
{
	if (atomic_set(&pairing_is_open, 0) != 0) {
		LOG_INF("Pairing window closed");
		ui_show_pairing(UI_PAIRING_CLOSED);
	}
}

void ble_svc_pairing_open(void)
{
	atomic_set(&pairing_is_open, 1);
	k_work_reschedule(&pairing_close_work, K_SECONDS(PAIRING_WINDOW_S));
	LOG_INF("Pairing window open for %d s", PAIRING_WINDOW_S);
	ui_show_pairing(UI_PAIRING_OPEN);
}

static void count_bond(const struct bt_bond_info *info, void *user_data)
{
	(*(int *)user_data)++;
}

static int bond_count(void)
{
	int n = 0;

	bt_foreach_bond(BT_ID_DEFAULT, count_bond, &n);
	return n;
}

static void disconnect_one(struct bt_conn *conn, void *user_data)
{
	bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

void ble_svc_forget_bonds(void)
{
	int err = bt_unpair(BT_ID_DEFAULT, NULL);

	LOG_INF("Forgot all pairings (%d)", err);
	bt_conn_foreach(BT_CONN_TYPE_LE, disconnect_one, NULL);
	ble_svc_pairing_open();
}

static enum bt_security_err pairing_accept(struct bt_conn *conn, const struct bt_conn_pairing_feat *const feat)
{
	if (atomic_get(&pairing_is_open) == 0) {
		LOG_WRN("Pairing refused: the pairing window is closed");
		return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
	}
	return BT_SECURITY_ERR_SUCCESS;
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	LOG_INF("Paired (bonded %d)", bonded);
	(void)k_work_cancel_delayable(&pairing_close_work);
	atomic_set(&pairing_is_open, 0);
	ui_show_pairing(UI_PAIRING_DONE);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	LOG_WRN("Pairing failed (%d)", reason);
}

static struct bt_conn_auth_cb auth_cb = {.pairing_accept = pairing_accept};
static struct bt_conn_auth_info_cb auth_info_cb = {.pairing_complete = pairing_complete, .pairing_failed = pairing_failed};

/* A bonded central gets its link encrypted at once; an unknown one only while pairing is allowed. */
static void secure_link(struct bt_conn *conn)
{
	if (bt_le_bond_exists(BT_ID_DEFAULT, bt_conn_get_dst(conn)) || atomic_get(&pairing_is_open) != 0) {
		int err = bt_conn_set_security(conn, BT_SECURITY_L2);

		if (err != 0) {
			LOG_WRN("Requesting security failed (%d)", err);
		}
	}
}

static int pairing_init(void)
{
	int err = bt_conn_auth_cb_register(&auth_cb);

	if (err == 0) {
		err = bt_conn_auth_info_cb_register(&auth_info_cb);
	}
	k_work_init_delayable(&pairing_close_work, pairing_close);
	return err;
}

/* After bt_enable() and the settings are loaded: with nothing paired yet, the first setup needs no button. */
static void pairing_start(void)
{
	settings_load_subtree("bt");
	if (bond_count() == 0) {
		ble_svc_pairing_open();
	}
}
#else
void ble_svc_pairing_open(void) {}
void ble_svc_forget_bonds(void) {}
static void secure_link(struct bt_conn *conn) {}
static int pairing_init(void) { return 0; }
static void pairing_start(void) {}
#endif

static void connected(struct bt_conn *conn, uint8_t err)
{
	int ret;

	if (err != 0) {
		LOG_ERR("Connection failed, err 0x%02x %s", err, bt_hci_err_to_str(err));
		return;
	}
	LOG_INF("Connected");
	/* A status gadget does not need a fast link: a slower connection saves radio time. The central may refuse. */
	ret = bt_conn_le_param_update(conn, BT_LE_CONN_PARAM(CONN_INTERVAL_MIN, CONN_INTERVAL_MAX, CONN_LATENCY,
							     CONN_TIMEOUT));
	if (ret != 0) {
		LOG_WRN("Connection parameter update failed (%d)", ret);
	}
	secure_link(conn);
	if (link_cb != NULL) {
		link_cb(true);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected, reason 0x%02x %s", reason, bt_hci_err_to_str(reason));
	if (link_cb != NULL) {
		link_cb(false);
	}
}

static void recycled(void)
{
	k_work_submit(&adv_work);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
};

int ble_svc_init(ble_event_cb_t on_event, ble_link_cb_t on_link, ble_usage_cb_t on_usage,
		 ble_time_cb_t on_time, ble_battery_cb_t on_battery)
{
	int err;

	event_cb = on_event;
	link_cb = on_link;
	usage_cb = on_usage;
	time_cb = on_time;
	battery_cb = on_battery;

	err = pairing_init();
	if (err != 0) {
		LOG_ERR("Pairing init failed (%d)", err);
		return err;
	}

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_ERR("Bluetooth init failed (%d)", err);
		return err;
	}

#ifdef CONFIG_BUDDY_SMP_UPDATE
	/* Bluetooth works, so a newly updated image can be updated again: keep it. */
	smp_update_confirm();
#endif

	/* The identity (and so the address the name is built from) can exist only once the settings are loaded. */
	pairing_start();
	name_init();
	k_work_init(&adv_work, adv_work_handler);
	k_work_submit(&adv_work);

	return 0;
}
