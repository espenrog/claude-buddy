#include "sound.h"

static const struct tone finished[] = {{880, 90}, {0, 40}, {1319, 150}};
static const struct tone needs_you[] = {{1047, 110}, {0, 70}, {1047, 110}, {0, 70}, {1568, 240}};
static const struct tone nudge[] = {{784, 140}, {0, 90}, {988, 200}};
static const struct tone limit[] = {{659, 200}, {0, 50}, {523, 200}, {0, 50}, {392, 420}};
static const struct tone unmuted[] = {{1047, 70}, {1568, 100}};
static const struct tone low_battery[] = {{784, 150}, {0, 60}, {659, 150}, {0, 60}, {523, 300}};
static const struct tone fall[] = {{1976, 60}, {1760, 60}, {1568, 60}, {1319, 60}, {1047, 60}, {880, 60}, {698, 120}};

#define SEQ(a) {a, sizeof(a) / sizeof(a[0])}

static const struct {
	const struct tone *tones;
	size_t count;
} sequences[SOUND_COUNT] = {
	[SOUND_FINISHED] = SEQ(finished),
	[SOUND_NEEDS_YOU] = SEQ(needs_you),
	[SOUND_NUDGE] = SEQ(nudge),
	[SOUND_LIMIT] = SEQ(limit),
	[SOUND_UNMUTED] = SEQ(unmuted),
	[SOUND_LOW_BATTERY] = SEQ(low_battery),
	[SOUND_FALL] = SEQ(fall),
};

const struct tone *sound_tones(enum sound id, size_t *count)
{
	if ((unsigned)id >= SOUND_COUNT) {
		*count = 0;
		return NULL;
	}
	*count = sequences[id].count;
	return sequences[id].tones;
}

int sound_for_transition(enum mood from, enum mood to)
{
	if (from == to) {
		return -1;
	}
	switch (to) {
	case MOOD_ALERT:
		return SOUND_NEEDS_YOU;
	case MOOD_EXCITED:
		return SOUND_FINISHED;
	case MOOD_LIMIT:
		return SOUND_LIMIT;
	case MOOD_SCARED:
		return SOUND_FALL;
	default:
		return -1;
	}
}

int sound_for_battery(enum battery_level from, enum battery_level to)
{
	return (to > from && to >= BATTERY_LOW) ? SOUND_LOW_BATTERY : -1;
}

uint32_t sound_nudge_delay_ms(unsigned done)
{
	static const uint32_t delays_ms[] = {60 * 1000, 2 * 60 * 1000, 5 * 60 * 1000};

	return done < sizeof(delays_ms) / sizeof(delays_ms[0]) ? delays_ms[done] : 0;
}

bool sound_audible(const struct buddy_cfg *cfg, int hour)
{
	if ((cfg->flags & BUDDY_CFG_FLAG_MUTED) != 0 || cfg->volume == 0) {
		return false;
	}
	return !(hour >= 0 && buddy_cfg_is_quiet(cfg, hour));
}

uint8_t sound_duty_percent(uint8_t volume)
{
	return (volume > 100 ? 100 : volume) / 2;
}
