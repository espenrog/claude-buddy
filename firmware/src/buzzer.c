#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

#include "buddy_clock.h"
#include "buzzer.h"
#include "cfg_store.h"

LOG_MODULE_REGISTER(buzzer, LOG_LEVEL_INF);

#define QUEUE_DEPTH 4

static struct buddy_clock clock_state;
static struct k_spinlock clock_lock;

#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), pwms)
#define HAVE_BUZZER 1
static const struct pwm_dt_spec pwm = PWM_DT_SPEC_GET(DT_PATH(zephyr_user));

K_MSGQ_DEFINE(sound_q, sizeof(uint8_t), QUEUE_DEPTH, 1);
static struct k_work_delayable step_work;
static const struct tone *playing;
static size_t playing_count, playing_index;
static uint8_t playing_duty;

static void silence(void)
{
	(void)pwm_set_pulse_dt(&pwm, 0);
}

static void step(struct k_work *work)
{
	ARG_UNUSED(work);

	if (playing == NULL || playing_index >= playing_count) {
		uint8_t id;

		silence();
		playing = NULL;
		if (k_msgq_get(&sound_q, &id, K_NO_WAIT) != 0) {
			return;
		}
		playing = sound_tones((enum sound)id, &playing_count);
		playing_index = 0;
		if (playing == NULL) {
			return;
		}
	}

	const struct tone *t = &playing[playing_index++];

	if (t->hz == 0 || playing_duty == 0) {
		silence();
	} else {
		uint32_t period_ns = NSEC_PER_SEC / t->hz;
		int err = pwm_set_dt(&pwm, period_ns, (uint32_t)((uint64_t)period_ns * playing_duty / 100));

		if (err != 0) {
			LOG_ERR("pwm_set failed (%d)", err);
		}
	}
	k_work_reschedule(&step_work, K_MSEC(t->ms));
}

int buzzer_init(void)
{
	if (!pwm_is_ready_dt(&pwm)) {
		LOG_ERR("Buzzer PWM not ready; no sound");
		return 0;
	}
	k_work_init_delayable(&step_work, step);
	silence();
	LOG_INF("Buzzer ready");
	return 0;
}

bool buzzer_present(void)
{
	return pwm_is_ready_dt(&pwm);
}

static void play(enum sound id, bool always)
{
	struct buddy_cfg cfg;
	int hour;

	if (!pwm_is_ready_dt(&pwm)) {
		return;
	}
	cfg_store_get(&cfg);
	K_SPINLOCK(&clock_lock) {
		hour = buddy_clock_hour(&clock_state, k_uptime_get());
	}
	if (!always && !sound_audible(&cfg, hour)) {
		return;
	}
	playing_duty = sound_duty_percent(cfg.volume);
	uint8_t v = (uint8_t)id;

	if (k_msgq_put(&sound_q, &v, K_NO_WAIT) != 0) {
		return; /* a burst of sounds: dropping one is fine */
	}
	if (playing == NULL) {
		k_work_reschedule(&step_work, K_NO_WAIT);
	}
}

void buzzer_play(enum sound id)
{
	play(id, false);
}

void buzzer_play_always(enum sound id)
{
	play(id, true);
}
#else
int buzzer_init(void)
{
	LOG_INF("No buzzer configured");
	return 0;
}

bool buzzer_present(void)
{
	return false;
}

void buzzer_play(enum sound id)
{
	ARG_UNUSED(id);
}

void buzzer_play_always(enum sound id)
{
	ARG_UNUSED(id);
}
#endif

int buzzer_set_time(const uint8_t *wire, size_t len)
{
	int err;

	K_SPINLOCK(&clock_lock) {
		err = buddy_clock_set_from_wire(&clock_state, wire, len, k_uptime_get());
	}
	return err;
}

int buzzer_clock_hour(void)
{
	int hour;

	K_SPINLOCK(&clock_lock) {
		hour = buddy_clock_hour(&clock_state, k_uptime_get());
	}
	return hour;
}
