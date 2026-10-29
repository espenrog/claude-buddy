/*
 * Hourly battery log: once an hour the buddy records the charge, the cell voltage, the average battery current and how
 * long the display was lit. The records are kept in flash (see battery_log_store.h) and read out over Bluetooth, so the
 * real use of a day can be seen without a power profiler. Pure logic (no Zephyr), testable on a host.
 */

#ifndef BATTERY_LOG_H_
#define BATTERY_LOG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BATTERY_LOG_PERIOD_MS (3600u * 1000u)
#define BATTERY_LOG_SLOTS 336 /* two weeks of hourly records */
#define BATTERY_LOG_HOUR_UNKNOWN 0xFF

#define BATTERY_LOG_FLAG_ON_USB 0x01  /* USB power or charging at some point in the hour: the current is not the buddy's use */
#define BATTERY_LOG_FLAG_PARTIAL 0x02 /* the hour was cut short (not used by the firmware yet; reserved) */
#define BATTERY_LOG_FLAG_MEASURE 0x04 /* a measurement mode held the buddy in one state at some point in the hour */

/* Wire format of one record: seq (u32 le), hour of day when it closed (0..23, 0xFF = unknown), flags, percent,
 * millivolts (u16 le), average battery current in units of 10 microamperes (i16 le, positive = charging), seconds the
 * display was lit (u16 le). */
#define BATTERY_LOG_RECORD_SIZE 13
/* What the log characteristic returns: newest seq (u32 le), number of records held (u16 le), then the selected record
 * if there is one. Stays within the 20 bytes of a default ATT read. */
#define BATTERY_LOG_READ_HEADER 6
#define BATTERY_LOG_READ_MAX (BATTERY_LOG_READ_HEADER + BATTERY_LOG_RECORD_SIZE)

struct battery_log_record {
	uint32_t seq; /* 1, 2, 3 ... and kept across restarts; 0 = none */
	uint8_t hour;
	uint8_t flags;
	uint8_t percent;
	uint16_t mv;
	int16_t avg_current_10ua;
	uint16_t lit_s;
};

/* The hour being collected. */
struct battery_log_acc {
	uint32_t elapsed_ms;
	uint32_t lit_ms;
	int64_t current_ua_ms; /* sum of current (microamperes, positive = charging) x time (ms) */
	uint8_t flags;
};

void battery_log_acc_init(struct battery_log_acc *acc);

/*
 * Adds a sample that covers the last `dt_ms`. When that completes an hour, fills *out (with seq 0: the ring numbers it)
 * and returns true; the next hour then starts empty. The charge and voltage in the record are those of the closing sample.
 */
bool battery_log_acc_add(struct battery_log_acc *acc, uint32_t dt_ms, int32_t current_ua, bool display_lit, bool on_usb,
			 bool measure, uint8_t percent, uint16_t mv, int hour, struct battery_log_record *out);

/* The records, newest overwriting oldest. */
struct battery_log_ring {
	struct battery_log_record slot[BATTERY_LOG_SLOTS];
	uint32_t newest; /* 0 = empty */
};

void battery_log_ring_init(struct battery_log_ring *ring);

/* Appends a record, giving it the next sequence number (stored into *rec). Returns its slot. */
unsigned battery_log_ring_add(struct battery_log_ring *ring, struct battery_log_record *rec);

/* Puts back a record read from flash (it keeps its seq). Returns false if it does not belong in the ring. */
bool battery_log_ring_restore(struct battery_log_ring *ring, const struct battery_log_record *rec);

/* The slot a sequence number lives in. */
static inline unsigned battery_log_slot_of(uint32_t seq)
{
	return (seq - 1) % BATTERY_LOG_SLOTS;
}

/* The record with that sequence number, or NULL if it was never written or is overwritten. */
const struct battery_log_record *battery_log_ring_get(const struct battery_log_ring *ring, uint32_t seq);

/* How many records are held. */
unsigned battery_log_ring_count(const struct battery_log_ring *ring);

void battery_log_record_to_wire(const struct battery_log_record *rec, uint8_t out[BATTERY_LOG_RECORD_SIZE]);
/* Returns 0, or -EINVAL for a wrong length or a sequence number of 0. */
int battery_log_record_from_wire(struct battery_log_record *rec, const uint8_t *buf, size_t len);

/* Builds the answer to a read when `selected` was written last: the header and, if held, that record. Returns the length. */
size_t battery_log_read_wire(const struct battery_log_ring *ring, uint32_t selected, uint8_t out[BATTERY_LOG_READ_MAX]);

#endif /* BATTERY_LOG_H_ */
