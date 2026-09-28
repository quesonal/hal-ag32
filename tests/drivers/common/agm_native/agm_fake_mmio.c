/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "agm_fake_mmio.h"

#include <errno.h>

#include <zephyr/init.h>
#include <zephyr/sys/util.h>

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

/*
 * The two regions every AGM peripheral lives in:
 *
 *  0x40000000 + 32 MiB  APB/AHB peripherals and the GPIO banks. Covers UART0
 *                       (0x40025000), I2C0/1 (0x4002b000+), SPI0/1
 *                       (0x40012000/0x40013000), GPTIMER0-4 (0x40020000+),
 *                       TIMER0/1 (0x4001e000+), GPIO0-9 (0x40014000+), DMAC0
 *                       (0x41000000) and CRC0 (0x41002000).
 *  0x03000000 + 1 MiB   SYS controller: clock gates and resets, which the
 *                       SoC's own pinctrl/soc glue touches.
 */
static const struct {
	uintptr_t base;
	size_t size;
} agm_fake_regions[] = {
	{ 0x40000000U, 0x02000000U },
	{ 0x03000000U, 0x00100000U },
};

bool agm_fake_mmio_range_is_free(uintptr_t base, size_t size)
{
	FILE *maps = fopen("/proc/self/maps", "r");
	char line[256];
	bool free_range = true;

	if (maps == NULL) {
		printf("agm-fake-mmio: no /proc/self/maps, cannot check 0x%lx\n",
		       (unsigned long)base);
		return false;
	}

	while (fgets(line, sizeof(line), maps) != NULL) {
		unsigned long lo = 0;
		unsigned long hi = 0;

		if (sscanf(line, "%lx-%lx", &lo, &hi) != 2) {
			continue;
		}
		if ((lo < (unsigned long)base + size) && (hi > (unsigned long)base)) {
			printf("agm-fake-mmio: 0x%lx+0x%zx overlaps %s",
			       (unsigned long)base, size, line);
			free_range = false;
			break;
		}
	}
	fclose(maps);

	return free_range;
}

int agm_fake_mmio_map(uintptr_t base, size_t size)
{
	void *p;

	/* MAP_FIXED silently replaces whatever is there, so refuse instead of
	 * clobbering a mapping the simulator itself is using. */
	if (!agm_fake_mmio_range_is_free(base, size)) {
		return -EEXIST;
	}

	p = mmap((void *)base, size, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (p == MAP_FAILED) {
		return -ENOMEM;
	}
	memset(p, 0, size);

	return 0;
}

static int agm_fake_mmio_init(void)
{
	for (size_t i = 0U; i < ARRAY_SIZE(agm_fake_regions); i++) {
		int ret = agm_fake_mmio_map(agm_fake_regions[i].base,
					    agm_fake_regions[i].size);

		if (ret != 0) {
			/* The suites assert on the registers anyway; failing here
			 * loudly beats a segfault inside a driver's init. */
			printf("agm-fake-mmio: 0x%lx failed: %d\n",
			       (unsigned long)agm_fake_regions[i].base, ret);
			return ret;
		}
	}

	return 0;
}

/* Before PRE_KERNEL_1: the drivers under test (and the SoC pinctrl they call)
 * touch these addresses from their own init. */
SYS_INIT(agm_fake_mmio_init, EARLY, 0);
