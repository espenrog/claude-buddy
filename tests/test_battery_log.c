/* Host-side unit tests for the hourly battery log. Built and run via tests/test_mood_c.py. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "battery_log.h"

static int failures;

#define CHECK(cond)                                                                        \
	do {                                                                               \
		if (!(cond)) {                                                             \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
			failures++;                                                        \
		}                                                                          \
	} while (0)

static struct battery_log_ring ring;

/* Runs `seconds` of samples every 10 s; returns true if a record closed (the last one is left in *out). */
static bool run(struct battery_log_acc *acc, unsigned seconds, int32_t ua, bool lit, bool usb, struct battery_log_record *out)
{
	bool closed = false;

	for (unsigned t = 0; t < seconds; t += 10) {
		closed |= battery_log_acc_add(acc, 10000, ua, lit, usb, false, 80, 3900, 14, out);
	}
	return closed;
}

static void test_average_and_display_time(void)
{
	struct battery_log_acc acc;
	struct battery_log_record rec;

	battery_log_acc_init(&acc);
	CHECK(!run(&acc, 1800, -2000, true, false, &rec)); /* half an hour lit at 2 mA out ... */
	CHECK(run(&acc, 1800, -200, false, false, &rec));  /* ... half an hour dark at 0.2 mA out */
	CHECK(rec.avg_current_10ua == -110);               /* -1.1 mA */
	CHECK(rec.lit_s == 1800);
	CHECK(rec.hour == 14 && rec.percent == 80 && rec.mv == 3900 && rec.flags == 0 && rec.seq == 0);
}

static void test_the_next_hour_starts_empty(void)
{
	struct battery_log_acc acc;
	struct battery_log_record rec;

	battery_log_acc_init(&acc);
	CHECK(run(&acc, 3600, -5000, true, true, &rec));
	CHECK(rec.flags & BATTERY_LOG_FLAG_ON_USB);
	CHECK(run(&acc, 3600, -100, false, false, &rec));
	CHECK(rec.avg_current_10ua == -10 && rec.lit_s == 0 && rec.flags == 0);
}

static void test_measure_flag(void)
{
	struct battery_log_acc acc;
	struct battery_log_record rec;

	battery_log_acc_init(&acc);
	CHECK(!battery_log_acc_add(&acc, 10000, -100, false, false, true, 80, 3900, 3, &rec)); /* one sample in a mode ... */
	CHECK(battery_log_acc_add(&acc, BATTERY_LOG_PERIOD_MS, -100, false, false, false, 80, 3900, 3, &rec));
	CHECK(rec.flags == BATTERY_LOG_FLAG_MEASURE); /* ... marks the whole hour */
}

static void test_unknown_hour_and_clamping(void)
{
	struct battery_log_acc acc;
	struct battery_log_record rec;

	battery_log_acc_init(&acc);
	CHECK(battery_log_acc_add(&acc, BATTERY_LOG_PERIOD_MS, -900000000, false, false, false, 50, 3700, -1, &rec));
	CHECK(rec.hour == BATTERY_LOG_HOUR_UNKNOWN);
	CHECK(rec.avg_current_10ua == INT16_MIN);
}

static void test_ring_numbers_and_overwrites(void)
{
	struct battery_log_record rec = {.percent = 1};

	battery_log_ring_init(&ring);
	CHECK(battery_log_ring_get(&ring, 1) == NULL);
	for (unsigned i = 1; i <= BATTERY_LOG_SLOTS + 5; i++) {
		rec.percent = (uint8_t)i;
		CHECK(battery_log_ring_add(&ring, &rec) == (i - 1) % BATTERY_LOG_SLOTS);
		CHECK(rec.seq == i);
	}
	CHECK(ring.newest == BATTERY_LOG_SLOTS + 5);
	CHECK(battery_log_ring_get(&ring, 5) == NULL); /* overwritten */
	CHECK(battery_log_ring_get(&ring, 6) != NULL && battery_log_ring_get(&ring, 6)->percent == 6);
	CHECK(battery_log_ring_get(&ring, ring.newest + 1) == NULL);
	CHECK(battery_log_ring_count(&ring) == BATTERY_LOG_SLOTS);
}

static void test_restore_in_any_order(void)
{
	struct battery_log_ring r;
	struct battery_log_record a = {.seq = 3}, b = {.seq = 3 + BATTERY_LOG_SLOTS}, c = {.seq = 2};
	struct battery_log_record next = {0};

	battery_log_ring_init(&r);
	CHECK(battery_log_ring_restore(&r, &b));
	CHECK(battery_log_ring_restore(&r, &a)); /* older, same slot: must not win */
	CHECK(battery_log_ring_restore(&r, &c));
	CHECK(!battery_log_ring_restore(&r, &(struct battery_log_record){0}));
	CHECK(r.newest == b.seq);
	CHECK(battery_log_ring_get(&r, b.seq) != NULL && battery_log_ring_get(&r, a.seq) == NULL);
	CHECK(battery_log_ring_get(&r, 2) != NULL);
	battery_log_ring_add(&r, &next);
	CHECK(next.seq == b.seq + 1); /* numbering carries on after a restart */
}

static void test_wire_format(void)
{
	struct battery_log_record rec = {.seq = 0x01020304, .hour = 23, .flags = 1, .percent = 77, .mv = 4012,
					 .avg_current_10ua = -123, .lit_s = 3600}, back;
	uint8_t wire[BATTERY_LOG_RECORD_SIZE], answer[BATTERY_LOG_READ_MAX];
	struct battery_log_record two = {.percent = 9};

	battery_log_record_to_wire(&rec, wire);
	CHECK(wire[0] == 4 && wire[3] == 1 && wire[4] == 23 && wire[7] == (4012 & 0xFF));
	CHECK(battery_log_record_from_wire(&back, wire, sizeof(wire)) == 0);
	CHECK(memcmp(&rec, &back, sizeof(rec)) == 0);
	CHECK(battery_log_record_from_wire(&back, wire, sizeof(wire) - 1) == -EINVAL);
	memset(wire, 0, 4);
	CHECK(battery_log_record_from_wire(&back, wire, sizeof(wire)) == -EINVAL);

	battery_log_ring_init(&ring);
	CHECK(battery_log_read_wire(&ring, 1, answer) == BATTERY_LOG_READ_HEADER); /* empty */
	CHECK(answer[0] == 0 && answer[4] == 0);
	battery_log_ring_add(&ring, &two);
	CHECK(battery_log_read_wire(&ring, 1, answer) == BATTERY_LOG_READ_MAX);
	CHECK(answer[0] == 1 && answer[4] == 1 && answer[BATTERY_LOG_READ_HEADER] == 1 && answer[BATTERY_LOG_READ_HEADER + 6] == 9);
	CHECK(battery_log_read_wire(&ring, 7, answer) == BATTERY_LOG_READ_HEADER); /* not there */
	CHECK(BATTERY_LOG_READ_MAX <= 20);
}

int main(void)
{
	test_average_and_display_time();
	test_the_next_hour_starts_empty();
	test_measure_flag();
	test_unknown_hour_and_clamping();
	test_ring_numbers_and_overwrites();
	test_restore_in_any_order();
	test_wire_format();
	if (failures == 0) {
		printf("all battery log tests passed\n");
	}
	return failures != 0;
}
