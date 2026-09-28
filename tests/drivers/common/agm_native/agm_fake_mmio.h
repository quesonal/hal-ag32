/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Back the AgRV2K peripheral address space with RAM, on native_sim.
 *
 * The AGM drivers reach their hardware through DT_INST_REG_ADDR, and native_sim
 * maps nothing there: a raw dereference at 0x40025000 is a host access to an
 * unmapped page -- nothing in /proc/self/maps covers that range.
 * Reserving the range with mmap(MAP_FIXED) at SYS_INIT(EARLY) turns it
 * into ordinary RAM, after which an *unmodified* driver runs and the test can
 * read back what it wrote.
 *
 * Linking agm_fake_mmio.c is all a suite has to do -- it registers its own
 * EARLY init -- plus keeping the *real* register addresses in its devicetree
 * overlay, so the numbers under test are the production ones.
 *
 * What it does NOT do is model behaviour: RAM answers reads with the last
 * written value, so any bit the driver *polls* (a DONE flag, a BUSY bit) has to
 * be driven by the test. That is the other half, and it belongs in each
 * suite next to the assertions that need it.
 */

#ifndef AGM_FAKE_MMIO_H_
#define AGM_FAKE_MMIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Map one region of the AgRV2K register space into RAM.
 *
 * @param base Region start (the real address).
 * @param size Region size.
 *
 * @retval 0   Mapped and zeroed.
 * @retval -EEXIST Something is already mapped there; nothing was touched.
 * @retval -ENOMEM mmap() refused.
 */
int agm_fake_mmio_map(uintptr_t base, size_t size);

/** @brief Is the host's mapping of [base, base+size) free? */
bool agm_fake_mmio_range_is_free(uintptr_t base, size_t size);

#endif /* AGM_FAKE_MMIO_H_ */
