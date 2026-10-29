#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "battery.h"
#include "battery_gauge.h"

LOG_MODULE_REGISTER(battery_gauge, LOG_LEVEL_INF);

#ifdef CONFIG_BUDDY_FUEL_GAUGE

#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>

#include <nrf_fuel_gauge.h>

#include "battery_log_store.h"
#include "buzzer.h"
#include "ui.h"

#define CHARGER_NODE DT_ALIAS(buddy_charger)
#define POLL_ON_BATTERY_S 10
#define POLL_ON_USB_S 5

/* The cell model comes from the SDK's sample unless you point BUDDY_BATTERY_MODEL_DIR at your own (see docs/FIRMWARE.md). */
static const struct battery_model model = {
#include "battery_model.inc"
};

static const struct device *const charger = DEVICE_DT_GET(CHARGER_NODE);

struct reading {
	float voltage; /* V */
	float current; /* A, negative = discharging (Zephyr convention) */
	float temp;    /* degrees C, from the cell's thermistor (25 if it cannot be read, for the fuel gauge) */
	bool temp_ok;  /* the thermistor was read */
	float die_temp; /* degrees C, the power chip itself; NaN if it cannot be read */
	int32_t chg_status;
	bool vbus;
};

static float sensor_to_float(const struct sensor_value *v)
{
	return (float)v->val1 + (float)v->val2 / 1000000.0f;
}

static int read_chip(struct reading *r)
{
	struct sensor_value v;
	int err = sensor_sample_fetch(charger);

	if (err != 0) {
		return err;
	}
	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &v);
	r->voltage = sensor_to_float(&v);
	r->temp_ok = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_TEMP, &v) == 0;
	r->temp = r->temp_ok ? sensor_to_float(&v) : 25.0f;
	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &v);
	r->current = sensor_to_float(&v);
	r->die_temp = sensor_channel_get(charger, SENSOR_CHAN_DIE_TEMP, &v) == 0 ? sensor_to_float(&v) : NAN;
	sensor_channel_get(charger, SENSOR_CHAN_NPM13XX_CHARGER_STATUS, &v);
	r->chg_status = v.val1;

	err = sensor_attr_get(charger, SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS, SENSOR_ATTR_NPM13XX_CHARGER_VBUS_PRESENT, &v);
	r->vbus = (err == 0) && (v.val1 != 0);
	return 0;
}

/* Tells the library what the charger is doing, as it needs for the state of health and the charge phase. */
static void inform_charge_state(int32_t chg_status)
{
	union nrf_fuel_gauge_ext_state_info_data info;

	if (chg_status & BATTERY_NPM_STATUS_COMPLETE) {
		info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_COMPLETE;
	} else if (chg_status & BATTERY_NPM_STATUS_TRICKLE) {
		info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_TRICKLE;
	} else if (chg_status & BATTERY_NPM_STATUS_CC) {
		/* "limited": the charge current may be held back by the USB limit or by temperature. */
		info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CC_LIMITED;
	} else if (chg_status & BATTERY_NPM_STATUS_CV) {
		info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CV;
	} else {
		info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_IDLE;
	}
	(void)nrf_fuel_gauge_ext_state_update(NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_STATE_CHANGE, &info);
}

static int start_gauge(const struct reading *r)
{
	struct sensor_value v;
	struct nrf_fuel_gauge_init_parameters params =
		NRF_FUEL_GAUGE_DEFAULT_INIT_PARAMETERS_SECONDARY(r->voltage, -r->current, r->temp, &model);
	float charge_limit;
	int err;

	/* The library counts current the other way round: negative = charging. */
	err = nrf_fuel_gauge_init(&params, NULL);
	if (err < 0) {
		return err;
	}
	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT, &v);
	charge_limit = sensor_to_float(&v);
	(void)nrf_fuel_gauge_ext_state_update(NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_CURRENT_LIMIT,
					      &(union nrf_fuel_gauge_ext_state_info_data){.charge_current_limit = charge_limit});
	(void)nrf_fuel_gauge_ext_state_update(NRF_FUEL_GAUGE_EXT_STATE_INFO_TERM_CURRENT,
					      &(union nrf_fuel_gauge_ext_state_info_data){.charge_term_current = charge_limit / 10.0f});
	inform_charge_state(r->chg_status);
	return 0;
}

static void gauge_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	bool running = false;
	bool vbus_before = false;
	int32_t status_before = 0;
	int64_t last_ms = 0;
	struct battery_state last_sent = {0};
	bool sent_once = false;
	struct battery_log_acc log_acc;
	int64_t log_ms = k_uptime_get();

	battery_log_acc_init(&log_acc);

	for (;;) {
		struct reading r;
		struct battery_state state = {0};
		float soc = 0.0f;
		uint16_t mv = 0;

		if (read_chip(&r) != 0) {
			LOG_WRN("Cannot read the power chip");
			k_sleep(K_SECONDS(POLL_ON_BATTERY_S));
			continue;
		}
		mv = (uint16_t)(r.voltage * 1000.0f);

		if (mv < BATTERY_PRESENT_MIN_MV || mv > 5000) {
			running = false; /* no battery: start again from scratch when one is connected */
		} else {
			if (!running) {
				if (start_gauge(&r) == 0) {
					running = true;
					vbus_before = r.vbus;
					status_before = r.chg_status;
					last_ms = k_uptime_get();
					LOG_INF("Fuel gauge started at %u mV", mv);
				} else {
					LOG_ERR("Fuel gauge did not start");
				}
			}
			if (running) {
				if (r.vbus != vbus_before) {
					vbus_before = r.vbus;
					(void)nrf_fuel_gauge_ext_state_update(
						r.vbus ? NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_CONNECTED
						       : NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_DISCONNECTED, NULL);
				}
				if (r.chg_status != status_before) {
					status_before = r.chg_status;
					inform_charge_state(r.chg_status);
				}
				int64_t now = k_uptime_get();
				float delta = (float)(now - last_ms) / 1000.0f;

				last_ms = now;
				if (nrf_fuel_gauge_process(r.voltage, -r.current, r.temp, delta, &soc, NULL) < 0) {
					LOG_ERR("Fuel gauge failed");
					running = false;
				}
			}
		}
		battery_state_from_gauge(&state, running ? soc : 0.0f, mv, r.chg_status);
		battery_state_set_power(&state, r.current, r.die_temp,
					IS_ENABLED(CONFIG_BUDDY_CELL_THERMISTOR) && r.temp_ok ? r.temp : NAN);
		if (!sent_once || battery_state_differs(&state, &last_sent)) {
			ui_set_battery(&state); /* only when something changed: every message wakes the display code */
			last_sent = state;
			sent_once = true;
		}
		if (mv >= BATTERY_PRESENT_MIN_MV && mv <= 5000) { /* the hourly log: only with a battery to log */
			struct battery_log_record rec;
			int64_t now = k_uptime_get();

			if (battery_log_acc_add(&log_acc, (uint32_t)(now - log_ms), (int32_t)(r.current * 1000000.0f), ui_display_lit(),
						r.vbus || state.charging || state.full, ui_measure_active(), state.percent, mv, buzzer_clock_hour(), &rec)) {
				battery_log_store_add(&rec);
			}
			log_ms = now;
		} else {
			battery_log_acc_init(&log_acc); /* no battery: the hour starts again when one is connected */
			log_ms = k_uptime_get();
		}
		LOG_DBG("%u mV, %d mA, %d C (die %d C), %u %%", mv, (int)(r.current * 1000.0f), (int)r.temp, (int)r.die_temp,
			state.percent);
		k_sleep(K_SECONDS(r.vbus ? POLL_ON_USB_S : POLL_ON_BATTERY_S));
	}
}

K_THREAD_DEFINE(battery_gauge_tid, 4096, gauge_thread, NULL, NULL, NULL, 10, 0, 2000);

int battery_gauge_init(void)
{
	if (!device_is_ready(charger)) {
		LOG_ERR("The power chip is not ready; no battery gauge");
		k_thread_abort(battery_gauge_tid);
		return 0;
	}
	LOG_INF("Battery gauge on");
	return 0;
}

#else /* no gauge on this board */

int battery_gauge_init(void)
{
	return 0;
}

#endif
