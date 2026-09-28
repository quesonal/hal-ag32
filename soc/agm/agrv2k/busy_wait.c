/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * arch_busy_wait() — k_busy_wait() on the core's own 64-bit cycle counter.
 *
 * SOC_AGM_AGRV2K selects CONFIG_ARCH_HAS_CUSTOM_BUSY_WAIT, so kernel/busy_wait.c
 * hands the wait to this function instead of the generic loop. Two reasons,
 * both:
 *
 *   - the generic loop converts microseconds to *timer* cycles with a 32-bit
 *     product (k_us_to_cyc_ceil32() -> usec * hz in uint32_t), so at 200 MHz
 *     anything above 2^32/200 = 21 474 836 us wraps and waits only the
 *     remainder: 21 574 836 us asked, 100 ms served, no diagnostic anywhere.
 *     `mcycle` is 64 bits and *is* the counter this loop checks, so there is
 *     no conversion to overflow -- the wait is expressed in elapsed cycles;
 *   - the frequency is resolved per call from SYS.CLK_CNTL, the way the
 *     vendor SDK's SYS_GetSysClkFreq() does it (framework-agrv_sdk/src/
 *     system.c) and its UTIL_IdleUs() spins (util.c). A wait issued while the
 *     core is on the RC clock -- the PRE_KERNEL_1 window the loader verifies
 *     the fabric in, and the inside of the PM stop window -- then lasts what
 *     was asked instead of declared/actual times of it.
 *
 * Two deliberate differences from the SDK's UTIL_IdleUs():
 *
 *   - the loop compares *elapsed* cycles (`now - start` against the target),
 *     not `now < start + target`; the SDK's absolute compare can exit early
 *     once, when the 64-bit counter wraps;
 *   - the target is rounded up (ceil), matching k_us_to_cyc_ceil32(): the
 *     contract is "at least this long".
 *
 * What this does *not* fix, and cannot: if the fabric's real clock differs
 * from the board devicetree's AGM_SYSCLK_HZ, `mcycle` still counts the real
 * clock while the conversion uses the declared one -- no counter on the chip
 * is an independent microsecond reference. That class of mismatch is refused
 * at flash time by tools/check_bitstream_clock.py.
 *
 * The 64-bit read needs `mcycleh` (CSR 0xB80); only the machine-mode CSRs are
 * implemented on this core (the unprivileged `cycle` alias at 0xC00 is an
 * illegal instruction here -- measured, mtval c0002973).
 */

#include <zephyr/arch/riscv/csr.h>
#include <zephyr/kernel.h>

#include "agm_sys.h"

#define CSR_MCYCLE  0xB00U
#define CSR_MCYCLEH 0xB80U

/*
 * HSI is the on-die RC clock the core runs on before FCB programs the PLL (and
 * inside the PM stop window). 10 MHz: the vendor SDK's BOARD_HSI_FREQUENCY and
 * the SDK's feature table; it is silicon, not a board choice, so
 * it is not a devicetree property.
 */
#define AGM_HSI_HZ	10000000U

/* The other two are the same devicetree numbers the rest of the SoC glue uses:
 * the pins node's crystal, and &sys's clocks phandle (= AGM_SYSCLK_HZ, what
 * CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC is built from). */
#define AGM_HSE_HZ	((uint32_t)DT_PROP(DT_NODELABEL(agrv2k_pins), hseclk_frequency))
#define AGM_SYSCLK_HZ	((uint32_t)DT_PROP_BY_PHANDLE(DT_NODELABEL(sys), clocks, \
							 clock_frequency))

static inline uint32_t sys_read(uint32_t off)
{
	return *((volatile uint32_t *)(AGM_SYS_BASE + off));
}

/* The core's cycle counter, read as a consistent 64-bit value: two high-word
 * reads have to agree, or the low word may have wrapped between them. */
static inline uint64_t mcycle_read(void)
{
	uint32_t hi, lo, hi_again;

	do {
		hi = csr_read(CSR_MCYCLEH);
		lo = csr_read(CSR_MCYCLE);
		hi_again = csr_read(CSR_MCYCLEH);
	} while (hi != hi_again);

	return ((uint64_t)hi << 32) | lo;
}

/* What one microsecond of waiting costs right now, in core cycles. */
static uint32_t agm_sysclk_hz(void)
{
	switch (sys_read(AGM_SYS_CLK_CNTL) & AGM_SYS_CLK_SOURCE_MASK) {
	case AGM_SYS_CLK_SOURCE_HSI:
		return AGM_HSI_HZ;
	case AGM_SYS_CLK_SOURCE_HSE:
		return AGM_HSE_HZ;
	default:
		/* PLL (and anything unexpected): the devicetree's SYSCLK. */
		return AGM_SYSCLK_HZ;
	}
}

void arch_busy_wait(uint32_t usec_to_wait)
{
	uint32_t hz, whole, rem;
	uint64_t want;

	if (usec_to_wait == 0U) {
		return;
	}

	/*
	 * ceil(usec * hz / 1e6), the rounding the generic path uses -- but
	 * without its 64-bit division, which on RV32 is a call to __udivdi3 and
	 * cost ~250 cycles of overshoot on *every* wait (the
	 * disassembly showed the call). Splitting hz into whole and remainder
	 * cycles per microsecond makes the common case one 32-bit multiply: every
	 * clock this SoC can select is a multiple of 1 MHz (200/100/10/8 MHz), so
	 * the remainder is normally zero. Rounding each part up keeps the result
	 * a lower bound of the request, like the kernel's ceil.
	 */
	hz = agm_sysclk_hz();            /* one SYS.CLK_CNTL read */
	whole = hz / 1000000U;
	rem = hz % 1000000U;
	want = (uint64_t)usec_to_wait * whole;
	if (rem != 0U) {
		want += ((uint64_t)usec_to_wait * rem + 999999ULL) / 1000000ULL;
	}

	if (want <= UINT32_MAX) {
		/* The common case: the target fits the low half of the counter, so
		 * an unsigned 32-bit difference is exact (it wraps with the counter
		 * and the comparison stays right). One CSR read per iteration --
		 * reading the high half too would cost ~150 extra cycles of
		 * overshoot per wait. */
		uint32_t start = (uint32_t)csr_read(CSR_MCYCLE);

		while (((uint32_t)csr_read(CSR_MCYCLE) - start) < (uint32_t)want) {
			/* spin: no yielding, no interrupts disabled -- same contract
			 * as the generic implementation. */
		}
		return;
	}

	/* Longer than 2^32 core cycles (~21.5 s at 200 MHz): the low half alone
	 * cannot tell "not yet" from "already wrapped", so count in 64 bits.
	 * This is the case the generic kernel loop gets wrong -- it converts in
	 * 32 bits and serves the wrapped remainder. */
	{
		uint64_t start = mcycle_read();

		while ((mcycle_read() - start) < want) {
		}
	}
}
