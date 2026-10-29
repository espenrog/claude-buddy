/* Test helper: reads hex strings (one per line) and reports whether firmware accepts them as settings
 * (or, with the argument "usage", as the usage characteristic). */
#include <stdio.h>
#include <string.h>

#include "battery.h"
#include "buddy_cfg.h"

int main(int argc, char **argv)
{
	int usage_mode = argc > 1 && strcmp(argv[1], "usage") == 0;
	int battery_mode = argc > 1 && strcmp(argv[1], "battery") == 0;
	char line[128];

	while (fgets(line, sizeof(line), stdin)) {
		uint8_t wire[32];
		size_t n = 0;

		for (char *p = line; p[0] && p[1] && p[0] != '\n' && n < sizeof(wire); p += 2) {
			unsigned v;
			if (sscanf(p, "%2x", &v) != 1) {
				break;
			}
			wire[n++] = (uint8_t)v;
		}
		if (battery_mode) {
			struct battery_state b;
			uint8_t bout[BATTERY_WIRE_SIZE_FULL];

			if (battery_from_wire(&b, wire, n) != 0) {
				printf("rejected\n");
				continue;
			}
			battery_to_wire(&b, bout);
			for (size_t i = 0; i < BATTERY_WIRE_SIZE; i++) { /* the test-write part; the rest is checked in logic_check */
				printf("%02x", bout[i]);
			}
			printf("\n");
			continue;
		}
		if (usage_mode) {
			struct buddy_usage u;
			uint8_t uout[BUDDY_USAGE_WIRE_SIZE_MAX];

			if (buddy_usage_from_wire(&u, wire, n) != 0) {
				printf("rejected\n");
				continue;
			}
			size_t m = buddy_usage_to_wire(&u, uout);

			for (size_t i = 0; i < m; i++) {
				printf("%02x", uout[i]);
			}
			printf("\n");
			continue;
		}

		struct buddy_cfg c;

		if (buddy_cfg_from_wire(&c, wire, n) != 0) {
			printf("rejected\n");
			continue;
		}
		uint8_t out[BUDDY_CFG_WIRE_SIZE];

		buddy_cfg_to_wire(&c, out);
		for (size_t i = 0; i < sizeof(out); i++) {
			printf("%02x", out[i]);
		}
		printf("\n");
	}
	return 0;
}
