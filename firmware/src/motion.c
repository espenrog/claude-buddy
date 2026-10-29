/*
 * The LSM6DS3TR-C does the watching in hardware: double tap, free fall and "wake up" (any movement from rest) raise its
 * INT1 pin, so the processor sleeps while the buddy sits still. A movement starts a short spell of reading samples, to
 * tell a shake (shake.c) from a pick-up. The registers are set directly: Zephyr's LSM6DSL driver has no tap or free-fall
 * interrupts.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "motion.h"
#include "shake.h"

LOG_MODULE_REGISTER(motion, LOG_LEVEL_INF);

#define IMU_NODE DT_ALIAS(imu0)

#define REG_WHO_AM_I 0x0F
#define REG_CTRL1_XL 0x10
#define REG_CTRL3_C 0x12
#define REG_WAKE_UP_SRC 0x1B
#define REG_TAP_SRC 0x1C
#define REG_OUTX_L_XL 0x28
#define REG_TAP_CFG 0x58
#define REG_TAP_THS_6D 0x59
#define REG_INT_DUR2 0x5A
#define REG_WAKE_UP_THS 0x5B
#define REG_WAKE_UP_DUR 0x5C
#define REG_FREE_FALL 0x5D
#define REG_MD1_CFG 0x5E

#define WHO_AM_I_VALUE 0x6A
#define CTRL3_SW_RESET 0x01
#define CTRL3_BDU_IF_INC 0x44           /* block data update, register address auto-increment */
#define CTRL1_XL_416HZ_4G 0x68          /* 416 Hz (what the tap detection wants), +-4 g, gyroscope stays off */
#define TAP_CFG_ON 0x8F                 /* interrupts on, taps on x, y and z, latched until read */
#define TAP_THS 0x06                    /* tap threshold: 6 x 4 g / 32 = 750 mg */
#define INT_DUR2_DOUBLE 0x7F            /* at 416 Hz: up to ~540 ms between taps, 29 ms quiet, 58 ms shock */
#define WAKE_UP_THS_DOUBLE_TAP BIT(7)
#define WAKE_UP_THS_MOVE 0x02           /* movement threshold: 2 x 4 g / 64 = 125 mg */
#define FREE_FALL_DUR 33                /* samples near 0 g before it counts as a fall: 33 / 416 Hz = ~80 ms */
#define FREE_FALL_THS_312MG 0x03
#define MD1_CFG_ROUTE (BIT(5) | BIT(4) | BIT(3)) /* wake-up, free fall and double tap on INT1 */

#define SRC_FF_IA BIT(5)  /* WAKE_UP_SRC: free fall */
#define SRC_WU_IA BIT(3)  /* WAKE_UP_SRC: movement */
#define SRC_DOUBLE BIT(4) /* TAP_SRC: double tap */

#define MG_PER_1000_LSB 122 /* at +-4 g */
#define BOOT_MS 20          /* the sensor's power-on time (15 ms) and some */
#define SAMPLE_MS 20
#define WATCH_MS 1500       /* how long to keep sampling after the last movement */
#define PICKUP_GAP_MS 2000  /* report "picked up" at most this often */
#define AFTER_FALL_MS 1000  /* the landing is not a shake */

#if DT_NODE_HAS_STATUS_OKAY(IMU_NODE)
static const struct i2c_dt_spec bus = I2C_DT_SPEC_GET(IMU_NODE);
static const struct gpio_dt_spec irq = GPIO_DT_SPEC_GET(IMU_NODE, irq_gpios);

static motion_cb_t callback;
static struct gpio_callback irq_cb;
static struct k_work irq_work;
static struct k_work_delayable sample_work;
static struct shake_det shake;
static int64_t watch_until_ms;
static int64_t last_pickup_ms = -PICKUP_GAP_MS;
static int64_t last_fall_ms = -AFTER_FALL_MS;

static int reg_write(uint8_t reg, uint8_t value)
{
	return i2c_reg_write_byte_dt(&bus, reg, value);
}

static void start_watching(int64_t now)
{
	if (now >= watch_until_ms) {
		shake_reset(&shake);
	}
	watch_until_ms = now + WATCH_MS;
	k_work_reschedule(&sample_work, K_NO_WAIT);
}

static void sample_handler(struct k_work *work)
{
	int64_t now = k_uptime_get();
	uint8_t raw[6];
	int16_t mg[3];

	if (now >= watch_until_ms) {
		return;
	}
	if (i2c_burst_read_dt(&bus, REG_OUTX_L_XL, raw, sizeof(raw)) != 0) {
		return;
	}
	for (int i = 0; i < 3; i++) {
		mg[i] = (int16_t)(((int32_t)(int16_t)(raw[2 * i] | (raw[2 * i + 1] << 8)) * MG_PER_1000_LSB) / 1000);
	}
	if (shake_feed(&shake, mg, now) && now - last_fall_ms >= AFTER_FALL_MS) {
		LOG_INF("Shake");
		callback(MOTION_SHAKE);
	}
	k_work_reschedule(&sample_work, K_MSEC(SAMPLE_MS));
}

/* Reads (and so clears) the latched sources; runs again while INT1 stays high, so no event is left stuck. */
static void irq_handler(struct k_work *work)
{
	for (int round = 0; round < 4; round++) {
		int64_t now = k_uptime_get();
		uint8_t wake_src;
		uint8_t tap_src;

		if (i2c_reg_read_byte_dt(&bus, REG_WAKE_UP_SRC, &wake_src) != 0 ||
		    i2c_reg_read_byte_dt(&bus, REG_TAP_SRC, &tap_src) != 0) {
			LOG_WRN("Accelerometer read failed");
			return;
		}
		if (wake_src & SRC_FF_IA) {
			LOG_INF("Fall");
			last_fall_ms = now;
			callback(MOTION_FALL);
		}
		if (tap_src & SRC_DOUBLE) {
			LOG_INF("Double tap");
			callback(MOTION_DOUBLE_TAP);
		}
		if (wake_src & SRC_WU_IA) {
			if (now - last_pickup_ms >= PICKUP_GAP_MS) {
				last_pickup_ms = now;
				callback(MOTION_PICKED_UP);
			}
			start_watching(now);
		}
		if (gpio_pin_get_dt(&irq) <= 0) {
			return;
		}
	}
}

static void irq_isr(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
	k_work_submit(&irq_work);
}

int motion_init(motion_cb_t cb)
{
	uint8_t id;
	int err;

	if (!i2c_is_ready_dt(&bus) || !gpio_is_ready_dt(&irq)) {
		return -ENODEV;
	}
	k_sleep(K_MSEC(BOOT_MS)); /* its supply (the power chip's LDO1) may just have come up */
	err = i2c_reg_read_byte_dt(&bus, REG_WHO_AM_I, &id);
	if (err != 0 || id != WHO_AM_I_VALUE) {
		LOG_WRN("No accelerometer (err %d, id 0x%02x)", err, err == 0 ? id : 0);
		return -ENODEV;
	}

	err = reg_write(REG_CTRL3_C, CTRL3_SW_RESET);
	k_sleep(K_MSEC(1));
	const uint8_t setup[][2] = {
		{REG_CTRL3_C, CTRL3_BDU_IF_INC},
		{REG_TAP_CFG, TAP_CFG_ON},
		{REG_TAP_THS_6D, TAP_THS},
		{REG_INT_DUR2, INT_DUR2_DOUBLE},
		{REG_WAKE_UP_THS, WAKE_UP_THS_DOUBLE_TAP | WAKE_UP_THS_MOVE},
		{REG_WAKE_UP_DUR, (FREE_FALL_DUR & BIT(5)) ? BIT(7) : 0},
		{REG_FREE_FALL, ((FREE_FALL_DUR & 0x1F) << 3) | FREE_FALL_THS_312MG},
		{REG_MD1_CFG, MD1_CFG_ROUTE},
		{REG_CTRL1_XL, CTRL1_XL_416HZ_4G},
	};

	for (size_t i = 0; i < ARRAY_SIZE(setup) && err == 0; i++) {
		err = reg_write(setup[i][0], setup[i][1]);
	}
	if (err != 0) {
		LOG_ERR("Accelerometer setup failed (%d)", err);
		return err;
	}

	callback = cb;
	k_work_init(&irq_work, irq_handler);
	k_work_init_delayable(&sample_work, sample_handler);
	err = gpio_pin_configure_dt(&irq, GPIO_INPUT);
	if (err == 0) {
		gpio_init_callback(&irq_cb, irq_isr, BIT(irq.pin));
		err = gpio_add_callback_dt(&irq, &irq_cb);
	}
	if (err == 0) {
		err = gpio_pin_interrupt_configure_dt(&irq, GPIO_INT_EDGE_TO_ACTIVE);
	}
	if (err != 0) {
		LOG_ERR("Accelerometer interrupt setup failed (%d)", err);
		return err;
	}
	k_work_submit(&irq_work); /* clear anything latched during setup */
	LOG_INF("Accelerometer ready");
	return 0;
}
#else
int motion_init(motion_cb_t cb)
{
	return -ENODEV;
}
#endif
