/* Firmware updates over Bluetooth inside the application (MCUboot swap, SMP server). */

#ifndef SMP_UPDATE_H_
#define SMP_UPDATE_H_

/* Marks a freshly swapped-in image as good, so MCUboot keeps it. Call once Bluetooth is up: an image that crashes
 * before this is replaced by the previous one at the next restart. Does nothing for an image already confirmed. */
void smp_update_confirm(void);

#endif /* SMP_UPDATE_H_ */
