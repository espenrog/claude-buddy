/*
 * Mood state machine for the Claude companion. Pure logic: time is passed in,
 * so it can be tested on a host without Zephyr.
 */

#ifndef MOOD_H_
#define MOOD_H_

#include <stdbool.h>
#include <stdint.h>

/* Event codes, written by the bridge to the BLE event characteristic. */
enum claude_event {
	CLAUDE_EVT_SESSION_STARTED = 0x01,
	CLAUDE_EVT_SESSION_ENDED = 0x02,
	CLAUDE_EVT_PROMPT_SUBMITTED = 0x03,
	CLAUDE_EVT_TURN_FINISHED = 0x04,
	CLAUDE_EVT_NEEDS_INPUT = 0x05,
	CLAUDE_EVT_ANSWERED = 0x06,
	CLAUDE_EVT_ERROR = 0x07,
	CLAUDE_EVT_LIMIT_REACHED = 0x08,
	CLAUDE_EVT_IDLE = 0x09,
};

enum mood {
	MOOD_SLEEPING,
	MOOD_CHILL,
	MOOD_BORED,
	MOOD_WORKING,
	MOOD_EXCITED,
	MOOD_ALERT,
	MOOD_CONFUSED,
	MOOD_ANGRY,
	MOOD_LIMIT,
	MOOD_DIZZY,  /* shaken (boards with an accelerometer) */
	MOOD_SCARED, /* dropped */
};

#define MOOD_POKES_FOR_ANGRY 5

/* Claude Code sessions the buddy tracks at once. The bridge assigns each session a slot. */
#define MOOD_MAX_SESSIONS 8

/* A session that has been silent this long is assumed to be gone (crashed, laptop closed). */
#define MOOD_SESSION_STALE_MS (60 * 60 * 1000)

struct mood_session {
	int64_t last_event_ms;
	bool active;
	bool working;
	bool needs_input;
};

struct mood_summary {
	uint8_t active;  /* sessions the buddy knows about */
	uint8_t working; /* ... of which are working */
	uint8_t waiting; /* ... of which wait for the user */
};

struct mood_ctx {
	struct mood_session sessions[MOOD_MAX_SESSIONS];
	int64_t last_activity_ms;
	int64_t last_prompt_ms;
	int64_t hold_until_ms;
	enum mood hold_mood;
	int64_t poke_ms[MOOD_POKES_FOR_ANGRY];
	uint8_t poke_count;
	bool limit;
};

void mood_init(struct mood_ctx *ctx, int64_t now_ms);

/* Returns 0, or -EINVAL for an unknown event code or a slot out of range. */
int mood_on_event(struct mood_ctx *ctx, uint8_t code, uint8_t slot, int64_t now_ms);

void mood_on_poke(struct mood_ctx *ctx, int64_t now_ms);

/* The accelerometer: shaken makes the buddy dizzy for a while, dropped makes it scared. */
void mood_on_shake(struct mood_ctx *ctx, int64_t now_ms);
void mood_on_fall(struct mood_ctx *ctx, int64_t now_ms);

/* Forgets sessions that have been silent too long. Call periodically before mood_get(). */
void mood_tick(struct mood_ctx *ctx, int64_t now_ms);

/* Resolves the current mood. Time-based moods (bored, asleep) need it called periodically. */
enum mood mood_get(const struct mood_ctx *ctx, int64_t now_ms);

void mood_summarize(const struct mood_ctx *ctx, struct mood_summary *out);

#endif /* MOOD_H_ */
