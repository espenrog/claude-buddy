#include <zephyr/dfu/mcuboot.h>
#include <zephyr/logging/log.h>

#include "smp_update.h"

LOG_MODULE_REGISTER(smp_update, LOG_LEVEL_INF);

void smp_update_confirm(void)
{
	if (boot_is_img_confirmed()) {
		return;
	}
	if (boot_write_img_confirmed() != 0) {
		LOG_ERR("Cannot confirm the new firmware; the bootloader will go back to the old one at the next restart");
		return;
	}
	LOG_INF("New firmware confirmed");
}
