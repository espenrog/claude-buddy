/* Host-side unit tests for the mood state machine. Build and run via tests/test_mood_c.py. */

#include <errno.h>
#include <stdio.h>

#include "mood.h"

static int failures;

#define CHECK(cond)                                                                        \
	do {                                                                               \
		if (!(cond)) {                                                             \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
			failures++;                                                        \
		}                                                                          \
	} while (0)

#define SEC(n) ((int64_t)(n) * 1000)
#define MIN(n) (SEC(60) * (n))

static struct mood_ctx c;

static void fresh(void)
{
	mood_init(&c, 0);
}

static void test_initial_is_chill(void)
{
	fresh();
	CHECK(mood_get(&c, 0) == MOOD_CHILL);
}

static void test_working_then_excited_then_chill(void)
{
	fresh();
	mood_on_event(&c, CLAUDE_EVT_PROMPT_SUBMITTED, 0, SEC(1));
	CHECK(mood_get(&c, SEC(2)) == MOOD_WORKING);
	mood_on_event(&c, CLAUDE_EVT_TURN_FINISHED, 0, SEC(10));
	CHECK(mood_get(&c, SEC(11)) == MOOD_EXCITED);
	CHECK(mood_get(&c, SEC(15)) == MOOD_EXCITED);
	CHECK(mood_get(&c, SEC(17)) == MOOD_CHILL); /* 6 s hold has passed */
}

static void test_alert_is_sticky_until_answered(void)
{
	fresh();
	mood_on_event(&c, CLAUDE_EVT_PROMPT_SUBMITTED, 0, SEC(1));
	mood_on_event(&c, CLAUDE_EVT_NEEDS_INPUT, 0, SEC(2));
	CHECK(mood_get(&c, SEC(3)) == MOOD_ALERT);
	CHECK(mood_get(&c, MIN(3)) == MOOD_ALERT);
	mood_on_event(&c, CLAUDE_EVT_ANSWERED, 0, MIN(3));
	CHECK(mood_get(&c, MIN(3)) == MOOD_WORKING);
}

static void test_turn_finished_clears_alert(void)
{
	fresh();
	mood_on_event(&c, CLAUDE_EVT_NEEDS_INPUT, 0, SEC(1));
	mood_on_event(&c, CLAUDE_EVT_TURN_FINISHED, 0, SEC(2));
	CHECK(mood_get(&c, SEC(3)) == MOOD_EXCITED);
}

static void test_error_is_confused_then_returns_to_working(void)
{
	fresh();
	mood_on_event(&c, CLAUDE_EVT_PROMPT_SUBMITTED, 0, SEC(1));
	mood_on_event(&c, CLAUDE_EVT_ERROR, 0, SEC(5));
	CHECK(mood_get(&c, SEC(6)) == MOOD_CONFUSED);
	CHECK(mood_get(&c, SEC(10)) == MOOD_WORKING); /* 4 s hold has passed */
}

static void test_limit_until_next_prompt(void)
{
	fresh();
	mood_on_event(&c, CLAUDE_EVT_LIMIT_REACHED, 0, SEC(1));
	CHECK(mood_get(&c, SEC(2)) == MOOD_LIMIT);
	CHECK(mood_get(&c, MIN(10)) == MOOD_LIMIT);
	mood_on_event(&c, CLAUDE_EVT_PROMPT_SUBMITTED, 0, MIN(10));
	CHECK(mood_get(&c, MIN(10)) == MOOD_WORKING);
}

static void test_priority_limit_over_alert_over_hold(void)
{
	fresh();
	mood_on_event(&c, CLAUDE_EVT_TURN_FINISHED, 0, SEC(1));
	mood_on_event(&c, CLAUDE_EVT_NEEDS_INPUT, 0, SEC(2));
	CHECK(mood_get(&c, SEC(3)) == MOOD_ALERT); /* alert beats the excited hold */
	mood_on_event(&c, CLAUDE_EVT_LIMIT_REACHED, 0, SEC(4));
	CHECK(mood_get(&c, SEC(5)) == MOOD_LIMIT); /* limit beats alert */
}

static void test_bored_and_sleeping_timers(void)
{
	fresh();
	CHECK(mood_get(&c, MIN(4)) == MOOD_CHILL);
	CHECK(mood_get(&c, MIN(5)) == MOOD_BORED);
	CHECK(mood_get(&c, MIN(29)) == MOOD_BORED);
	CHECK(mood_get(&c, MIN(30)) == MOOD_SLEEPING);
	mood_on_event(&c, CLAUDE_EVT_PROMPT_SUBMITTED, 0, MIN(31));
	CHECK(mood_get(&c, MIN(31)) == MOOD_WORKING);
}

static void test_idle_does_not_count_as_activity(void)
{
	fresh();
	mood_on_event(&c, CLAUDE_EVT_IDLE, 0, MIN(29));
	CHECK(mood_get(&c, MIN(30)) == MOOD_SLEEPING);
}

static void test_session_events_reset_state(void)
{
	fresh();
	mood_on_event(&c, CLAUDE_EVT_NEEDS_INPUT, 0, SEC(1));
	mood_on_event(&c, CLAUDE_EVT_SESSION_STARTED, 0, SEC(2));
	CHECK(mood_get(&c, SEC(3)) == MOOD_CHILL);
	mood_on_event(&c, CLAUDE_EVT_PROMPT_SUBMITTED, 0, SEC(4));
	mood_on_event(&c, CLAUDE_EVT_SESSION_ENDED, 0, SEC(5));
	CHECK(mood_get(&c, SEC(6)) == MOOD_CHILL);
}

static void test_unknown_event_rejected(void)
{
	fresh();
	CHECK(mood_on_event(&c, 0x00, 0, SEC(1)) == -EINVAL);
	CHECK(mood_on_event(&c, 0x7f, 0, SEC(1)) == -EINVAL);
	CHECK(mood_get(&c, SEC(2)) == MOOD_CHILL);
}

static void test_five_pokes_in_four_seconds_make_angry(void)
{
	fresh();
	for (int i = 0; i < 5; i++) {
		mood_on_poke(&c, SEC(10) + i * 800); /* 0.8 s apart: 3.2 s total */
	}
	CHECK(mood_get(&c, SEC(14)) == MOOD_ANGRY);
	CHECK(mood_get(&c, SEC(20)) == MOOD_CHILL); /* 5 s hold has passed */
}

static void test_slow_pokes_do_not_anger(void)
{
	fresh();
	for (int i = 0; i < 8; i++) {
		mood_on_poke(&c, SEC(10) + i * 1500); /* 1.5 s apart: never 5 within 4 s */
	}
	CHECK(mood_get(&c, SEC(22)) != MOOD_ANGRY);
}

/* ---- several sessions ---- */

static void test_two_sessions_one_waiting_is_an_alert(void)
{
	struct mood_summary sum;

	fresh();
	mood_on_event(&c, CLAUDE_EVT_PROMPT_SUBMITTED, 0, SEC(1));
	mood_on_event(&c, CLAUDE_EVT_PROMPT_SUBMITTED, 1, SEC(2));
	CHECK(mood_get(&c, SEC(3)) == MOOD_WORKING);
	mood_on_event(&c, CLAUDE_EVT_NEEDS_INPUT, 1, SEC(4));
	CHECK(mood_get(&c, SEC(5)) == MOOD_ALERT);
	mood_summarize(&c, &sum);
	CHECK(sum.active == 2 && sum.working == 1 && sum.waiting == 1);
	mood_on_event(&c, CLAUDE_EVT_ANSWERED, 1, SEC(6));
	CHECK(mood_get(&c, SEC(7)) == MOOD_WORKING);
}

static void test_working_until_every_session_has_finished(void)
{
	fresh();
	mood_on_event(&c, CLAUDE_EVT_PROMPT_SUBMITTED, 0, SEC(1));
	mood_on_event(&c, CLAUDE_EVT_PROMPT_SUBMITTED, 1, SEC(2));
	mood_on_event(&c, CLAUDE_EVT_TURN_FINISHED, 0, SEC(10));
	CHECK(mood_get(&c, SEC(20)) == MOOD_WORKING); /* excited hold over; session 1 still works */
	mood_on_event(&c, CLAUDE_EVT_TURN_FINISHED, 1, SEC(30));
	CHECK(mood_get(&c, SEC(40)) == MOOD_CHILL);
}

static void test_a_waiting_session_is_not_cleared_by_another_one_finishing(void)
{
	fresh();
	mood_on_event(&c, CLAUDE_EVT_NEEDS_INPUT, 0, SEC(1));
	mood_on_event(&c, CLAUDE_EVT_PROMPT_SUBMITTED, 1, SEC(2));
	mood_on_event(&c, CLAUDE_EVT_TURN_FINISHED, 1, SEC(3));
	CHECK(mood_get(&c, SEC(4)) == MOOD_ALERT);
}

static void test_session_end_frees_the_slot(void)
{
	struct mood_summary sum;

	fresh();
	mood_on_event(&c, CLAUDE_EVT_NEEDS_INPUT, 2, SEC(1));
	mood_on_event(&c, CLAUDE_EVT_SESSION_ENDED, 2, SEC(2));
	mood_summarize(&c, &sum);
	CHECK(sum.active == 0 && sum.waiting == 0);
	CHECK(mood_get(&c, SEC(3)) == MOOD_CHILL);
}

static void test_limit_stops_every_session_and_clears_on_a_new_prompt(void)
{
	struct mood_summary sum;

	fresh();
	mood_on_event(&c, CLAUDE_EVT_PROMPT_SUBMITTED, 0, SEC(1));
	mood_on_event(&c, CLAUDE_EVT_PROMPT_SUBMITTED, 3, SEC(2));
	mood_on_event(&c, CLAUDE_EVT_LIMIT_REACHED, 0, SEC(3));
	CHECK(mood_get(&c, SEC(4)) == MOOD_LIMIT);
	mood_summarize(&c, &sum);
	CHECK(sum.active == 2 && sum.working == 0);
	mood_on_event(&c, CLAUDE_EVT_PROMPT_SUBMITTED, 3, SEC(5));
	CHECK(mood_get(&c, SEC(6)) == MOOD_WORKING);
}

static void test_a_silent_session_is_forgotten_after_an_hour(void)
{
	struct mood_summary sum;

	fresh();
	mood_on_event(&c, CLAUDE_EVT_NEEDS_INPUT, 4, SEC(1)); /* then the laptop is closed */
	mood_tick(&c, MIN(59));
	CHECK(mood_get(&c, MIN(59)) == MOOD_ALERT);
	mood_tick(&c, MIN(61));
	mood_summarize(&c, &sum);
	CHECK(sum.active == 0);
	CHECK(mood_get(&c, MIN(61)) != MOOD_ALERT);
}

static void test_events_for_an_unseen_session_register_it(void)
{
	struct mood_summary sum;

	fresh();
	mood_on_event(&c, CLAUDE_EVT_ANSWERED, 5, SEC(1)); /* the bridge started mid-session */
	mood_summarize(&c, &sum);
	CHECK(sum.active == 1 && sum.working == 1);
}

static void test_slot_out_of_range_is_rejected(void)
{
	fresh();
	CHECK(mood_on_event(&c, CLAUDE_EVT_PROMPT_SUBMITTED, MOOD_MAX_SESSIONS, SEC(1)) == -EINVAL);
	CHECK(mood_on_event(&c, CLAUDE_EVT_PROMPT_SUBMITTED, 255, SEC(1)) == -EINVAL);
	CHECK(mood_get(&c, SEC(2)) == MOOD_CHILL);
}

static void test_shake_is_dizzy_then_back(void)
{
	fresh();
	mood_on_event(&c, CLAUDE_EVT_PROMPT_SUBMITTED, 0, SEC(1));
	mood_on_shake(&c, SEC(2));
	CHECK(mood_get(&c, SEC(3)) == MOOD_DIZZY);
	CHECK(mood_get(&c, SEC(7)) == MOOD_WORKING); /* 4 s hold has passed */
	mood_on_event(&c, CLAUDE_EVT_NEEDS_INPUT, 0, SEC(8));
	mood_on_shake(&c, SEC(9));
	CHECK(mood_get(&c, SEC(10)) == MOOD_ALERT); /* a waiting session is not hidden by a shake */
}

static void test_fall_is_scared_and_a_shake_replaces_it(void)
{
	fresh();
	mood_on_fall(&c, SEC(1));
	CHECK(mood_get(&c, SEC(2)) == MOOD_SCARED);
	CHECK(mood_get(&c, SEC(4)) == MOOD_CHILL); /* 2.5 s hold has passed */
	mood_on_fall(&c, SEC(10));
	mood_on_shake(&c, SEC(11));
	CHECK(mood_get(&c, SEC(12)) == MOOD_DIZZY);
}

int main(void)
{
	test_initial_is_chill();
	test_working_then_excited_then_chill();
	test_alert_is_sticky_until_answered();
	test_turn_finished_clears_alert();
	test_error_is_confused_then_returns_to_working();
	test_limit_until_next_prompt();
	test_priority_limit_over_alert_over_hold();
	test_bored_and_sleeping_timers();
	test_idle_does_not_count_as_activity();
	test_session_events_reset_state();
	test_unknown_event_rejected();
	test_five_pokes_in_four_seconds_make_angry();
	test_slow_pokes_do_not_anger();
	test_two_sessions_one_waiting_is_an_alert();
	test_working_until_every_session_has_finished();
	test_a_waiting_session_is_not_cleared_by_another_one_finishing();
	test_session_end_frees_the_slot();
	test_limit_stops_every_session_and_clears_on_a_new_prompt();
	test_a_silent_session_is_forgotten_after_an_hour();
	test_events_for_an_unseen_session_register_it();
	test_slot_out_of_range_is_rejected();
	test_shake_is_dizzy_then_back();
	test_fall_is_scared_and_a_shake_replaces_it();

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all mood tests passed\n");
	return 0;
}
