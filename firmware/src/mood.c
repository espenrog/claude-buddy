#include <errno.h>
#include <string.h>

#include "mood.h"

#define SLEEP_AFTER_MS (30 * 60 * 1000)
#define BORED_AFTER_MS (5 * 60 * 1000)
#define EXCITED_HOLD_MS 6000
#define CONFUSED_HOLD_MS 4000
#define ANGRY_HOLD_MS 5000
#define ANGRY_WINDOW_MS 4000
#define DIZZY_HOLD_MS 4000
#define SCARED_HOLD_MS 2500

static void hold(struct mood_ctx *ctx, enum mood mood, int64_t now_ms, int64_t duration_ms)
{
	ctx->hold_mood = mood;
	ctx->hold_until_ms = now_ms + duration_ms;
}

void mood_init(struct mood_ctx *ctx, int64_t now_ms)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->last_activity_ms = now_ms;
	ctx->last_prompt_ms = now_ms;
}

int mood_on_event(struct mood_ctx *ctx, uint8_t code, uint8_t slot, int64_t now_ms)
{
	struct mood_session *s;

	if (slot >= MOOD_MAX_SESSIONS) {
		return -EINVAL;
	}
	s = &ctx->sessions[slot];

	switch (code) {
	case CLAUDE_EVT_SESSION_STARTED:
		memset(s, 0, sizeof(*s));
		s->active = true;
		ctx->limit = false;
		ctx->last_activity_ms = now_ms;
		ctx->last_prompt_ms = now_ms;
		break;
	case CLAUDE_EVT_SESSION_ENDED:
		memset(s, 0, sizeof(*s));
		ctx->last_activity_ms = now_ms;
		return 0;
	case CLAUDE_EVT_PROMPT_SUBMITTED:
		s->working = true;
		s->needs_input = false;
		ctx->limit = false;
		ctx->last_activity_ms = now_ms;
		ctx->last_prompt_ms = now_ms;
		break;
	case CLAUDE_EVT_TURN_FINISHED:
		s->working = false;
		s->needs_input = false;
		ctx->last_activity_ms = now_ms;
		hold(ctx, MOOD_EXCITED, now_ms, EXCITED_HOLD_MS);
		break;
	case CLAUDE_EVT_NEEDS_INPUT:
		s->needs_input = true;
		ctx->last_activity_ms = now_ms;
		break;
	case CLAUDE_EVT_ANSWERED:
		s->needs_input = false;
		s->working = true;
		ctx->last_activity_ms = now_ms;
		break;
	case CLAUDE_EVT_ERROR:
		ctx->last_activity_ms = now_ms;
		hold(ctx, MOOD_CONFUSED, now_ms, CONFUSED_HOLD_MS);
		break;
	case CLAUDE_EVT_LIMIT_REACHED:
		/* The usage limit belongs to the account, so it stops every session. */
		for (int i = 0; i < MOOD_MAX_SESSIONS; i++) {
			ctx->sessions[i].working = false;
		}
		ctx->limit = true;
		ctx->last_activity_ms = now_ms;
		break;
	case CLAUDE_EVT_IDLE:
		/* Claude is waiting for the user; not new activity. */
		s->working = false;
		break;
	default:
		return -EINVAL;
	}

	/* Any event proves the session exists, even if we missed its start. */
	s->active = true;
	s->last_event_ms = now_ms;

	return 0;
}

void mood_on_poke(struct mood_ctx *ctx, int64_t now_ms)
{
	if (ctx->poke_count == MOOD_POKES_FOR_ANGRY) {
		memmove(&ctx->poke_ms[0], &ctx->poke_ms[1],
			sizeof(ctx->poke_ms[0]) * (MOOD_POKES_FOR_ANGRY - 1));
		ctx->poke_count--;
	}
	ctx->poke_ms[ctx->poke_count++] = now_ms;

	if (ctx->poke_count == MOOD_POKES_FOR_ANGRY &&
	    now_ms - ctx->poke_ms[0] <= ANGRY_WINDOW_MS) {
		hold(ctx, MOOD_ANGRY, now_ms, ANGRY_HOLD_MS);
		ctx->poke_count = 0;
	}
}

void mood_on_shake(struct mood_ctx *ctx, int64_t now_ms)
{
	hold(ctx, MOOD_DIZZY, now_ms, DIZZY_HOLD_MS);
}

void mood_on_fall(struct mood_ctx *ctx, int64_t now_ms)
{
	hold(ctx, MOOD_SCARED, now_ms, SCARED_HOLD_MS);
}

void mood_tick(struct mood_ctx *ctx, int64_t now_ms)
{
	for (int i = 0; i < MOOD_MAX_SESSIONS; i++) {
		struct mood_session *s = &ctx->sessions[i];

		if (s->active && now_ms - s->last_event_ms >= MOOD_SESSION_STALE_MS) {
			memset(s, 0, sizeof(*s));
		}
	}
}

void mood_summarize(const struct mood_ctx *ctx, struct mood_summary *out)
{
	memset(out, 0, sizeof(*out));
	for (int i = 0; i < MOOD_MAX_SESSIONS; i++) {
		const struct mood_session *s = &ctx->sessions[i];

		if (!s->active) {
			continue;
		}
		out->active++;
		if (s->needs_input) {
			out->waiting++;
		} else if (s->working) {
			out->working++;
		}
	}
}

enum mood mood_get(const struct mood_ctx *ctx, int64_t now_ms)
{
	struct mood_summary sum;

	mood_summarize(ctx, &sum);

	if (ctx->limit) {
		return MOOD_LIMIT;
	}
	if (sum.waiting > 0) {
		return MOOD_ALERT;
	}
	if (now_ms < ctx->hold_until_ms) {
		return ctx->hold_mood;
	}
	if (sum.working > 0) {
		return MOOD_WORKING;
	}
	if (now_ms - ctx->last_activity_ms >= SLEEP_AFTER_MS) {
		return MOOD_SLEEPING;
	}
	if (now_ms - ctx->last_prompt_ms >= BORED_AFTER_MS) {
		return MOOD_BORED;
	}

	return MOOD_CHILL;
}
