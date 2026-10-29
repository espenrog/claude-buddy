#include <errno.h>
#include <string.h>

#include "battery_log.h"

void battery_log_acc_init(struct battery_log_acc *acc)
{
	memset(acc, 0, sizeof(*acc));
}

static int16_t clamp_i16(int64_t v)
{
	return v > INT16_MAX ? INT16_MAX : (v < INT16_MIN ? INT16_MIN : (int16_t)v);
}

bool battery_log_acc_add(struct battery_log_acc *acc, uint32_t dt_ms, int32_t current_ua, bool display_lit, bool on_usb,
			 bool measure, uint8_t percent, uint16_t mv, int hour, struct battery_log_record *out)
{
	acc->elapsed_ms += dt_ms;
	acc->current_ua_ms += (int64_t)current_ua * dt_ms;
	if (display_lit) {
		acc->lit_ms += dt_ms;
	}
	if (on_usb) {
		acc->flags |= BATTERY_LOG_FLAG_ON_USB;
	}
	if (measure) {
		acc->flags |= BATTERY_LOG_FLAG_MEASURE;
	}
	if (acc->elapsed_ms < BATTERY_LOG_PERIOD_MS) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->hour = (hour >= 0 && hour <= 23) ? (uint8_t)hour : BATTERY_LOG_HOUR_UNKNOWN;
	out->flags = acc->flags;
	out->percent = percent;
	out->mv = mv;
	out->avg_current_10ua = clamp_i16(acc->current_ua_ms / (int64_t)acc->elapsed_ms / 10);
	out->lit_s = (uint16_t)(acc->lit_ms / 1000u > UINT16_MAX ? UINT16_MAX : acc->lit_ms / 1000u);
	battery_log_acc_init(acc);
	return true;
}

void battery_log_ring_init(struct battery_log_ring *ring)
{
	memset(ring, 0, sizeof(*ring));
}

unsigned battery_log_ring_add(struct battery_log_ring *ring, struct battery_log_record *rec)
{
	unsigned slot;

	rec->seq = ++ring->newest;
	slot = battery_log_slot_of(rec->seq);
	ring->slot[slot] = *rec;
	return slot;
}

bool battery_log_ring_restore(struct battery_log_ring *ring, const struct battery_log_record *rec)
{
	if (rec->seq == 0) {
		return false;
	}
	struct battery_log_record *slot = &ring->slot[battery_log_slot_of(rec->seq)];

	if (rec->seq > slot->seq) { /* flash can hand them back in any order: the newer one keeps the slot */
		*slot = *rec;
	}
	if (rec->seq > ring->newest) {
		ring->newest = rec->seq;
	}
	return true;
}

const struct battery_log_record *battery_log_ring_get(const struct battery_log_ring *ring, uint32_t seq)
{
	const struct battery_log_record *rec;

	if (seq == 0 || seq > ring->newest) {
		return NULL;
	}
	rec = &ring->slot[battery_log_slot_of(seq)];
	return rec->seq == seq ? rec : NULL;
}

unsigned battery_log_ring_count(const struct battery_log_ring *ring)
{
	unsigned n = 0;

	for (unsigned i = 0; i < BATTERY_LOG_SLOTS; i++) {
		if (ring->slot[i].seq != 0) {
			n++;
		}
	}
	return n;
}

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void battery_log_record_to_wire(const struct battery_log_record *rec, uint8_t out[BATTERY_LOG_RECORD_SIZE])
{
	put_u32(out, rec->seq);
	out[4] = rec->hour;
	out[5] = rec->flags;
	out[6] = rec->percent;
	out[7] = (uint8_t)rec->mv;
	out[8] = (uint8_t)(rec->mv >> 8);
	out[9] = (uint8_t)(uint16_t)rec->avg_current_10ua;
	out[10] = (uint8_t)((uint16_t)rec->avg_current_10ua >> 8);
	out[11] = (uint8_t)rec->lit_s;
	out[12] = (uint8_t)(rec->lit_s >> 8);
}

int battery_log_record_from_wire(struct battery_log_record *rec, const uint8_t *buf, size_t len)
{
	if (len != BATTERY_LOG_RECORD_SIZE || get_u32(buf) == 0) {
		return -EINVAL;
	}
	rec->seq = get_u32(buf);
	rec->hour = buf[4];
	rec->flags = buf[5];
	rec->percent = buf[6];
	rec->mv = (uint16_t)(buf[7] | (buf[8] << 8));
	rec->avg_current_10ua = (int16_t)(uint16_t)(buf[9] | (buf[10] << 8));
	rec->lit_s = (uint16_t)(buf[11] | (buf[12] << 8));
	return 0;
}

size_t battery_log_read_wire(const struct battery_log_ring *ring, uint32_t selected, uint8_t out[BATTERY_LOG_READ_MAX])
{
	const struct battery_log_record *rec = battery_log_ring_get(ring, selected);
	unsigned count = battery_log_ring_count(ring);

	put_u32(out, ring->newest);
	out[4] = (uint8_t)count;
	out[5] = (uint8_t)(count >> 8);
	if (rec == NULL) {
		return BATTERY_LOG_READ_HEADER;
	}
	battery_log_record_to_wire(rec, out + BATTERY_LOG_READ_HEADER);
	return BATTERY_LOG_READ_MAX;
}
