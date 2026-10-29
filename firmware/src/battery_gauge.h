/*
 * Battery gauge for boards with an nPM1300 power chip (the XIAO nRF54LM20A): reads the cell and the charger every few
 * seconds, runs Nordic's fuel gauge library and hands the result to the battery screen (ui_set_battery).
 * Boards without the chip build a stub. Needs the "buddy-charger" alias in the board overlay.
 */

#ifndef BATTERY_GAUGE_H_
#define BATTERY_GAUGE_H_

/* Starts the gauge thread. Returns 0 also when this board has no gauge. */
int battery_gauge_init(void);

#endif /* BATTERY_GAUGE_H_ */
