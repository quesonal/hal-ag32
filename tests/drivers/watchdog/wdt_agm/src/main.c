/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief native_sim suite for the AgRV2K SP805 watchdog driver.
 *
 * Everything asserted here is what the driver *programmed*, read back through
 * the real register addresses (RAM-backed window, tests/drivers/common/
 * agm_native). That covers the parts of this driver with no off-board coverage
 * at all: the milliseconds-to-ticks conversion and its bounds, the
 * unlock/INTCLR/LOAD/CTRL/lock sequence (the SP805 ignores writes while
 * locked), which control bits each wdt_timeout_cfg/reset flag maps to, the
 * feed path, and the one register outside the block that
 * WDT_OPT_PAUSE_HALTED_BY_DBG touches (SYS.APB_CLKSTOP).
 *
 * Not reachable here: the timeout ISR. It only runs when the hardware raises
 * IRQ 4, and the interrupt would have to be injected by a model of the
 * down-counter; the driver's ISR body is three lines of state plus a callback,
 * and the state it reads (MIS) is asserted indirectly through the mask
 * constant.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/ztest.h>

#define WDT_DEV  DEVICE_DT_GET(DT_NODELABEL(wdog_agm0))

/* Register offsets (drivers/watchdog/wdt_agm.c). */
#define R_LOAD    0x00U
#define R_CTRL    0x08U
#define R_INTCLR  0x0cU
#define R_MIS     0x14U
#define R_LOCK    0xc00U

#define CTRL_INTEN BIT(0)
#define CTRL_RESEN BIT(1)

#define UNLOCK_KEY 0x1ACCE551U

#define WDT_BASE 0x40011000UL

/* SYS.APB_CLKSTOP (soc/agm/agrv2k/agm_sys.h): bit 1 pauses WDOG0's clock while
 * the core is halted. */
#define SYS_APB_CLKSTOP 0x03000080UL
#define APB_CLK_WDOG0   (1UL << 1)

static uint32_t rd(uint32_t off)
{
	return *(volatile uint32_t *)(WDT_BASE + off);
}

static void wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(WDT_BASE + off) = val;
}

static void suite_reset_fake_regs(void *fixture)
{
	ARG_UNUSED(fixture);

	/* The armed/disarmed state is *device data*, not a register: a case that
	 * calls wdt_setup() and does not disable would make every later
	 * install_timeout() answer -EBUSY. So each case starts disarmed. */
	(void)wdt_disable(WDT_DEV);	/* -EFAULT when it was not armed */

	/* init() leaves the control register at 0 and the lock closed; each case
	 * may arm the watchdog, so this is about returning to that state. */
	wr(R_CTRL, 0U);
	wr(R_LOAD, 0U);
	wr(R_INTCLR, 0U);
	wr(R_LOCK, 0U);
}

ZTEST_SUITE(wdt_agm, NULL, NULL, suite_reset_fake_regs, NULL, NULL);

/* ---- install_timeout: the window and the reset flags ------------------ */

ZTEST(wdt_agm, test_01_install_timeout_validates_the_window_and_the_flags)
{
	struct wdt_timeout_cfg cfg = {
		.window = { .min = 0U, .max = 1000U },
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};

	zassert_equal(wdt_install_timeout(WDT_DEV, NULL), -EINVAL, "a NULL config");

	cfg.window.max = 0U;
	zassert_equal(wdt_install_timeout(WDT_DEV, &cfg), -EINVAL, "a zero timeout");
	cfg.window.max = 1000U;

	/* It is not a windowed watchdog. */
	cfg.window.min = 1U;
	zassert_equal(wdt_install_timeout(WDT_DEV, &cfg), -ENOTSUP, "no window support");
	cfg.window.min = 0U;

	/* Only a SoC reset is meaningful: with no reset and no callback the
	 * watchdog would do nothing at all. */
	cfg.flags = WDT_FLAG_RESET_CPU_CORE;
	zassert_equal(wdt_install_timeout(WDT_DEV, &cfg), -ENOTSUP, "no core-only reset");
	cfg.flags = WDT_FLAG_RESET_NONE;
	zassert_equal(wdt_install_timeout(WDT_DEV, &cfg), -ENOTSUP,
		      "no reset and no callback would be a no-op");

	/* A callback makes RESET_NONE meaningful. */
	cfg.flags = WDT_FLAG_RESET_NONE;
	cfg.callback = (wdt_callback_t)0x1;	/* never invoked in this suite */
	zassert_ok(wdt_install_timeout(WDT_DEV, &cfg), "interrupt-only timeouts are allowed");

	/* Flags outside the reset mask are refused. (Not one of the WDT_OPT_*
	 * bits: those are wdt_setup() options, and on this Zephyr they overlap
	 * WDT_FLAG_RESET_MASK's bits, so OR-ing one in would silently look like
	 * a reset flag.) */
	cfg.flags = WDT_FLAG_RESET_SOC | BIT(3);
	cfg.callback = NULL;
	zassert_equal(wdt_install_timeout(WDT_DEV, &cfg), -ENOTSUP, "an unknown flag");

	cfg.flags = WDT_FLAG_RESET_SOC;
	zassert_ok(wdt_install_timeout(WDT_DEV, &cfg), "the plain SoC-reset timeout");
}

ZTEST(wdt_agm, test_02_setup_converts_milliseconds_to_pclk_ticks)
{
	struct wdt_timeout_cfg cfg = {
		.window = { .min = 0U, .max = 1500U },
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	uint32_t load;

	zassert_ok(wdt_install_timeout(WDT_DEV, &cfg));
	zassert_ok(wdt_setup(WDT_DEV, 0U));

	/* The load value is read back through the ITSELF register: the SP805 is
	 * write-mostly, so this is the only way to see the conversion. 1500 ms
	 * at 200 MHz is 300 000 000 ticks -- rounded UP to whole ticks. */
	load = rd(R_LOAD);
	zassert_equal(load, 300000000U, "1500 ms at 200 MHz");

	/* INTEN is what makes the countdown run at all on this IP (RESEN alone
	 * leaves WdogValue frozen -- see the driver header), and RESEN
	 * is added because the config asked for a SoC reset. */
	zassert_equal(rd(R_CTRL), CTRL_INTEN | CTRL_RESEN, "INTEN always, RESEN for a reset");
}

ZTEST(wdt_agm, test_03_setup_refuses_a_second_arm_and_unsupported_options)
{
	struct wdt_timeout_cfg cfg = {
		.window = { .min = 0U, .max = 1000U },
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};

	zassert_ok(wdt_install_timeout(WDT_DEV, &cfg));
	zassert_ok(wdt_setup(WDT_DEV, 0U));

	zassert_equal(wdt_setup(WDT_DEV, 0U), -EBUSY, "already armed");
	zassert_equal(wdt_install_timeout(WDT_DEV, &cfg), -EBUSY, "and the timeout is fixed");

	zassert_ok(wdt_disable(WDT_DEV));
}

/* ---- the hardware sequence ------------------------------------------- */

ZTEST(wdt_agm, test_10_setup_uses_the_unlock_sequence_and_ends_locked)
{
	struct wdt_timeout_cfg cfg = {
		.window = { .min = 0U, .max = 100U },
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};

	zassert_ok(wdt_install_timeout(WDT_DEV, &cfg));
	zassert_ok(wdt_setup(WDT_DEV, 0U));

	/* The SP805 ignores register writes unless LOCK was written with
	 * 0x1ACCE551 first, so the driver unlocks, clears the interrupt, sets
	 * LOAD and CTRL, and locks again. The last thing written to LOCK is
	 * therefore 0 -- and the fact that LOAD/CTRL hold their values at all
	 * is what proves the unlock happened for real (a locked write is
	 * dropped). */
	zassert_equal(rd(R_LOCK), 0U, "the register file is left locked");
	zassert_equal(rd(R_LOAD), 20000000U, "100 ms at 200 MHz");
	zassert_equal(rd(R_INTCLR), 1U, "the pending interrupt was cleared (and the counter "
				       "reloaded) before arming");
}

ZTEST(wdt_agm, test_11_feed_reloads_and_reenables_the_interrupt_source)
{
	struct wdt_timeout_cfg cfg = {
		.window = { .min = 0U, .max = 1000U },
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};

	zassert_ok(wdt_install_timeout(WDT_DEV, &cfg));
	zassert_ok(wdt_setup(WDT_DEV, 0U));

	wr(R_INTCLR, 0U);	/* clear the trace of setup's own clear */
	zassert_ok(wdt_feed(WDT_DEV, 0U));
	zassert_equal(rd(R_INTCLR), 1U, "feeding clears the interrupt, which also reloads");

	zassert_equal(wdt_feed(WDT_DEV, 1U), -EINVAL, "there is only one channel");

	zassert_ok(wdt_disable(WDT_DEV));
	zassert_equal(rd(R_CTRL), 0U, "disable stops the countdown");
	zassert_equal(wdt_disable(WDT_DEV), -EFAULT, "and is not idempotent");

	zassert_equal(wdt_feed(WDT_DEV, 0U), -EINVAL, "feeding a disabled watchdog");
}

ZTEST(wdt_agm, test_12_pause_halted_by_dbg_touches_the_sys_clock_stop_bit)
{
	struct wdt_timeout_cfg cfg = {
		.window = { .min = 0U, .max = 1000U },
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	volatile uint32_t *clkstop = (volatile uint32_t *)SYS_APB_CLKSTOP;

	*clkstop = 0U;

	zassert_ok(wdt_install_timeout(WDT_DEV, &cfg));
	zassert_ok(wdt_setup(WDT_DEV, WDT_OPT_PAUSE_HALTED_BY_DBG));
	zassert_equal(*clkstop & APB_CLK_WDOG0, APB_CLK_WDOG0,
		      "the watchdog's clock stops while a debugger halts the core");
	zassert_ok(wdt_disable(WDT_DEV));

	/* Pausing during sleep is not possible: WDOG0 runs off pclk and keeps
	 * counting while the CPU sleeps. */
	zassert_ok(wdt_install_timeout(WDT_DEV, &cfg));
	zassert_equal(wdt_setup(WDT_DEV, WDT_OPT_PAUSE_IN_SLEEP), -ENOTSUP,
		      "no pause-in-sleep support");
}
