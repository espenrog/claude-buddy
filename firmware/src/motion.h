/* The board's accelerometer (an LSM6DS3TR-C on the XIAO nRF54LM20A): taps, shakes, being picked up and falling. */

#ifndef MOTION_H_
#define MOTION_H_

enum motion_event {
	MOTION_DOUBLE_TAP, /* two quick knocks on the case: counts as a poke */
	MOTION_PICKED_UP,  /* moved from rest: wakes a dark display */
	MOTION_SHAKE,      /* shaken back and forth: the buddy gets dizzy */
	MOTION_FALL,       /* dropped (free fall): the buddy gets scared */
};

typedef void (*motion_cb_t)(enum motion_event event);

/* Returns 0, -ENODEV when the board has no accelerometer (or it does not answer), or another negative error. */
int motion_init(motion_cb_t cb);

#endif /* MOTION_H_ */
