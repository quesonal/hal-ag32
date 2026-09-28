/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AgRV2K clock tree — HSI / HSE / PLL source switching.
 *
 * Mirrors AgRV SDK's framework-agrv_sdk/src/system.c (the clock bits).
 * Why this is its own file:
 *   The clock-switch sequence is purely a SYS-controller concern,
 *   independent of FCB bitstream injection (fcb.c) and peripheral pin
 *   mux (pinctrl.c). Pulling it out lets soc.c read as a clean
 *   orchestration layer.
 *
 * SDK reference:
 *   framework-agrv_sdk/src/system.c::SYS_SwitchHSIClock   (lines 31-35)
 *   framework-agrv_sdk/src/system.c::SYS_SwitchPLLClock   (lines 42-95)
 *   framework-agrv_sdk/src/system.c::SYS_SetSclkAuto      (lines 99-104)
 *
 * PLL behaviour:
 *   - SDK spins forever in HSE_RDY/PLL_RDY waits; we follow suit. With
 *     FCB properly programmed (fcb.c) the PLL locks deterministically,
 *     so an unbounded wait is acceptable.
 *   - The PLL_RDY wait uses the SDK's 5000-iter toggle retry pattern
 *     (system.c:81-89) so a transient bitstream glitch does not deadlock.
 */

#include <zephyr/init.h>

#include "agm_sys.h"

/* SYS controller address + offsets are shared with the drivers
 * (agm_sys.h —). */
#define SYS_BASE              AGM_SYS_BASE

/* CLK_CNTL's offset and bits live in agm_sys.h: arch_busy_wait()
 * (busy_wait.c) reads the same source-select field, and SYS addresses are
 * supposed to have exactly one definition . */

/* SCLK divider (FLASH timing). Per AgRV SDK system.h:91-97. */
#define SYS_SCLK_DIV_MASK         0xfU
#define SYS_SCLK_DIV_HIGH_OFFSET  8U
#define SYS_SCLK_DIV_LOW_OFFSET   12U
#define SYS_SCLK_DIV_HIGH(d)      (((d) & SYS_SCLK_DIV_MASK) << SYS_SCLK_DIV_HIGH_OFFSET)
#define SYS_SCLK_DIV_LOW(d)       (((d) & SYS_SCLK_DIV_MASK) << SYS_SCLK_DIV_LOW_OFFSET)
#define SYS_SCLK_DIV_HIGH_MASK    SYS_SCLK_DIV_HIGH(SYS_SCLK_DIV_MASK)
#define SYS_SCLK_DIV_LOW_MASK     SYS_SCLK_DIV_LOW(SYS_SCLK_DIV_MASK)

static inline uint32_t sys_read(uint32_t off)
{
	return *((volatile uint32_t *)(SYS_BASE + off));
}

static inline void sys_write(uint32_t off, uint32_t val)
{
	*((volatile uint32_t *)(SYS_BASE + off)) = val;
}

/*
 * Mirror AgRV SDK's SYS_SwitchHSIClock (system.c:31-35):
 * force source = HSI, then turn off HSE and PLL. Order matters:
 * the new source must be live before the old oscillators are dropped.
 */
void agrv2k_clk_switch_hsi(void)
{
	uint32_t clk = sys_read(AGM_SYS_CLK_CNTL);

	clk  = (clk & ~AGM_SYS_CLK_SOURCE_MASK) | AGM_SYS_CLK_SOURCE_HSI;
	clk &= ~(AGM_SYS_CLK_PLL_ON | AGM_SYS_CLK_HSE_ON);
	sys_write(AGM_SYS_CLK_CNTL, clk);
}

/*
 * Mirror AgRV SDK's SYS_SwitchPLLClock (system.c:42-95) +
 * SYS_SetSclkAuto (system.c:99-104), ported to Zephyr with no exit
 * code. SDK spins forever; we follow suit (with the SDK's
 * PLL_RDY toggle retry so a transient bitstream glitch cannot deadlock).
 *
 * pll_hz       — target PLL output frequency (Hz).
 * flash_max_hz — FLASH controller ceiling (Hz). SCLK divider is
 *                computed as (pll_hz - 1) / flash_max_hz so FLASH
 *                stays within its electrical limit.
 */
void agrv2k_clk_switch_pll(uint32_t pll_hz, uint32_t flash_max_hz)
{
	uint32_t clk;
	uint32_t retry;
	const uint32_t pll_retry_timeout = 5000U;

	/* Pre-step: set SCLK divider so FLASH sees at most flash_max_hz.
	 * Same formula as AgRV SDK SYS_SetSclkAuto (system.c:99-104).
	 */
	{
		uint8_t divider = (uint8_t)((pll_hz - 1U) / flash_max_hz);

		clk  = sys_read(AGM_SYS_CLK_CNTL);
		clk &= ~(SYS_SCLK_DIV_HIGH_MASK | SYS_SCLK_DIV_LOW_MASK);
		clk |=  SYS_SCLK_DIV_HIGH(divider) | SYS_SCLK_DIV_LOW(divider);
		sys_write(AGM_SYS_CLK_CNTL, clk);
	}

	/* Turn on HSE + PLL. 407 board's HSE source is BYPASS_OFF (per
	 * example_board.ve HSECLK 8, no bypass).
	 */
	clk  = sys_read(AGM_SYS_CLK_CNTL);
	clk &= ~AGM_SYS_CLK_HSE_BYP;
	clk |=  AGM_SYS_CLK_HSE_ON | AGM_SYS_CLK_PLL_ON;
	sys_write(AGM_SYS_CLK_CNTL, clk);

	/* Wait HSE ready. */
	while ((sys_read(AGM_SYS_CLK_CNTL) & AGM_SYS_CLK_HSE_RDY) == 0U) {
		/* spin */
	}

	/* Wait PLL ready, with the SDK's 5000-iter toggle retry. */
	retry = 0U;
	while ((sys_read(AGM_SYS_CLK_CNTL) & AGM_SYS_CLK_PLL_RDY) == 0U) {
		if (++retry > pll_retry_timeout) {
			retry = 0U;
			sys_write(AGM_SYS_CLK_CNTL, clk & ~AGM_SYS_CLK_PLL_ON);
			sys_write(AGM_SYS_CLK_CNTL, clk);
		}
	}

	/* Switch source last (avoid HSI/HSE glitch). */
	clk = (sys_read(AGM_SYS_CLK_CNTL) & ~AGM_SYS_CLK_SOURCE_MASK) | AGM_SYS_CLK_SOURCE_PLL;
	sys_write(AGM_SYS_CLK_CNTL, clk);
}
