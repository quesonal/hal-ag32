/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AGM AgRV2K system watchdog (WDOG0) driver.
 *
 * Hardware: SP805-style Arm-primecell watchdog at 0x40011000 (SDK
 * framework-agrv_sdk/src/watchdog.h):
 *   WdogLoad   0x00   reload value (counter counts down at pclk)
 *   WdogValue  0x04   current countdown value (RO)
 *   WdogControl 0x08  INTEN bit0 / RESEN bit1
 *   WdogIntClr 0x0C   write 1: clear the interrupt AND reload the
 *                     counter from WdogLoad (SDK WDOG_Feed/ClearInt)
 *   WdogRIS    0x10   raw interrupt status
 *   WdogMIS    0x14   masked interrupt status
 *   WdogLock   0xC00  0x1ACCE551 unlocks register access; any other
 *                     value (SDK writes 0) locks it again.
 *
 * The unit counts at pclk == SYSCLK (node `clocks` -> clk0), so the
 * load value for a 1 s timeout is one second of pclk ticks — the SDK
 * WDOG_Init1S()/WDOG_InitMS() use SYS_GetPclkFreq() exactly the same
 * way. PLIC IRQ 4, APB clock gate bit 1 (APB_MASK_WATCHDOG0).
 *
 * Fire semantics (the SDK TestWdog example only ever programs
 * INTEN+RESEN together):
 *   - The countdown only runs while INTEN is set: with RESEN alone
 *     (control = 0x2) WdogValue stays frozen at WdogLoad and nothing
 *     ever fires (verified by register readback). So this driver
 *     always sets INTEN and uses RESEN to select the reset action.
 *   - INTEN only:   the counter reaching zero raises the WDOG0 PLIC
 *                    interrupt. With a callback installed the ISR
 *                    masks the source and hands over to the callback;
 *                    wdt_feed() (WdogIntClr) clears the pending
 *                    interrupt, reloads the counter and re-enables
 *                    the source.
 *   - INTEN+RESEN:  additionally, if the timeout interrupt is still
 *                    pending when the counter reaches zero again, the
 *                    SoC resets (the SDK "fail after N feeds" model -
 *                    its ISR stops calling WDOG_ClearInt to let the
 *                    watchdog fire). A timeout with no callback
 *                    installed is left unserviced, so the SoC resets
 *                    on the next un-fed timeout.
 * Exactly one timeout (channel 0) is supported, as in the SDK.
 *
 * Notes:
 *   - wdt_feed() performs SDK WDOG_Feed() == WDOG_ClearInt(): unlock,
 *     WdogIntClr = 1 (clears any pending interrupt and reloads the
 *     counter), lock.
 *   - WDT_OPT_PAUSE_HALTED_BY_DBG is honored via the SYS APB_CLKSTOP
 *     register bit 1 (APB_MASK_WATCHDOG0): while the core is halted
 *     by a debugger the watchdog clock stops, so the watchdog cannot
 *     fire during a debug session. This mirrors the SDK TestWdog
 *     SYS_EnableAPBClkStop(APB_MASK_WATCHDOG0) usage.
 *   - The watchdog module owns its own reset domain: after a watchdog
 *     reset the control register comes back 0 (disabled), so a
 *     re-flashed image does not reset-loop (verify on target).
 */

#define DT_DRV_COMPAT agm_agrv2k_wdt

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

#include <agm_sys.h>

LOG_MODULE_REGISTER(wdt_agm, CONFIG_WDT_LOG_LEVEL);

/* SYS controller APB clock gate / debug clock stop (AltaRiscv.h
 * APB_MASK_WATCHDOG0 = bit 1 of APB_CLKENABLE / APB_CLKSTOP).
 * Addresses/offsets come from agm_sys.h (shared with the other drivers
 * and soc.c —).
 */
#define AGM_APB_WDOG0		AGM_APB_CLK_WDOG0

/* WDOG0 register offsets (SDK watchdog.h). */
#define AGM_WDOG_LOAD		0x00U
#define AGM_WDOG_VALUE		0x04U
#define AGM_WDOG_CTRL		0x08U
#define AGM_WDOG_INTCLR		0x0cU
#define AGM_WDOG_RIS		0x10U
#define AGM_WDOG_MIS		0x14U
#define AGM_WDOG_LOCK		0xc00U

#define AGM_WDOG_CTRL_INTEN	BIT(0)
#define AGM_WDOG_CTRL_RESEN	BIT(1)
#define AGM_WDOG_INT_MSK	BIT(0)

#define AGM_WDOG_UNLOCK_KEY	0x1ACCE551U
#define AGM_WDOG_MAX_LOAD	UINT32_MAX

struct agm_wdt_cfg {
	uint32_t base;
	uint32_t pclk_hz;
	uint32_t irq;
	void (*irq_config)(void);
};

struct agm_wdt_data {
	wdt_callback_t callback;
	uint32_t load;		/* reload value in pclk ticks */
	bool reset_soc;		/* cfg->flags requested a SoC reset */
	bool setup_done;
	bool irq_masked;	/* source masked after an unserviced timeout */
};

static inline uint32_t agm_wdt_read(const struct agm_wdt_cfg *cfg,
				    uint32_t reg)
{
	return sys_read32(cfg->base + reg);
}

static inline void agm_wdt_write(const struct agm_wdt_cfg *cfg,
				 uint32_t reg, uint32_t val)
{
	sys_write32(val, cfg->base + reg);
}

static void agm_wdt_unlock(const struct agm_wdt_cfg *cfg)
{
	agm_wdt_write(cfg, AGM_WDOG_LOCK, AGM_WDOG_UNLOCK_KEY);
}

static void agm_wdt_lock(const struct agm_wdt_cfg *cfg)
{
	agm_wdt_write(cfg, AGM_WDOG_LOCK, 0U);
}

/* SDK WDOG_Feed() == WDOG_ClearInt(): clears the interrupt and reloads
 * the counter with WdogLoad. */
static void agm_wdt_feed_hw(const struct agm_wdt_cfg *cfg)
{
	agm_wdt_unlock(cfg);
	agm_wdt_write(cfg, AGM_WDOG_INTCLR, 1U);
	agm_wdt_lock(cfg);
}

static int agm_wdt_install_timeout(const struct device *dev,
				   const struct wdt_timeout_cfg *cfg)
{
	const struct agm_wdt_cfg *dcfg = dev->config;
	struct agm_wdt_data *data = dev->data;
	uint64_t load64;
	uint32_t reset_flags;

	if (cfg == NULL || cfg->window.max == 0U) {
		return -EINVAL;
	}
	if (data->setup_done) {
		return -EBUSY;
	}

	/* Windowed (min > 0) timeouts are not supported by this IP. */
	if (cfg->window.min != 0U) {
		return -ENOTSUP;
	}

	reset_flags = cfg->flags & WDT_FLAG_RESET_MASK;
	if (reset_flags == WDT_FLAG_RESET_CPU_CORE) {
		return -ENOTSUP;
	}
	if (reset_flags == WDT_FLAG_RESET_NONE && cfg->callback == NULL) {
		/* Neither reset nor interrupt: the watchdog would do
		 * nothing. */
		return -ENOTSUP;
	}
	if ((cfg->flags & ~WDT_FLAG_RESET_MASK) != 0U) {
		return -ENOTSUP;
	}

	/* Timeout is rounded up to whole pclk ticks. pclk/1000 is exact
	 * for 200 MHz, but use 64-bit math for generality. */
	load64 = ((uint64_t)dcfg->pclk_hz * cfg->window.max + 999U) / 1000U;
	if (load64 == 0U || load64 > AGM_WDOG_MAX_LOAD) {
		return -EINVAL;
	}

	data->callback = cfg->callback;
	data->reset_soc = (reset_flags == WDT_FLAG_RESET_SOC);
	data->load = (uint32_t)load64;

	LOG_DBG("timeout %u ms -> load %u (pclk %u Hz)",
		cfg->window.max, data->load, dcfg->pclk_hz);
	return 0; /* channel 0 */
}

static int agm_wdt_setup(const struct device *dev, uint8_t options)
{
	const struct agm_wdt_cfg *cfg = dev->config;
	struct agm_wdt_data *data = dev->data;
	uint32_t ctrl;

	if (data->setup_done) {
		return -EBUSY;
	}

	/* Pausing in sleep is not supported: WDOG0 runs off pclk and
	 * keeps counting while the CPU sleeps. */
	if (options & WDT_OPT_PAUSE_IN_SLEEP) {
		return -ENOTSUP;
	}

	/* Stop the watchdog clock while the core is halted by the
	 * debugger (SDK SYS_EnableAPBClkStop(APB_MASK_WATCHDOG0)). */
	if (options & WDT_OPT_PAUSE_HALTED_BY_DBG) {
		sys_set_bits(AGM_SYS_BASE + AGM_SYS_APB_CLKSTOP, AGM_APB_WDOG0);
	} else {
		sys_clear_bits(AGM_SYS_BASE + AGM_SYS_APB_CLKSTOP, AGM_APB_WDOG0);
	}

	/* INTEN must stay set for the countdown to run at all (RESEN-only
	 * leaves the counter frozen); RESEN selects
	 * whether an unserviced timeout resets the SoC. */
	ctrl = AGM_WDOG_CTRL_INTEN;
	if (data->reset_soc) {
		ctrl |= AGM_WDOG_CTRL_RESEN;
	}

	agm_wdt_unlock(cfg);
	agm_wdt_write(cfg, AGM_WDOG_INTCLR, 1U);
	agm_wdt_write(cfg, AGM_WDOG_LOAD, data->load);
	agm_wdt_write(cfg, AGM_WDOG_CTRL, ctrl);
	agm_wdt_lock(cfg);

	data->setup_done = true;
	data->irq_masked = false;
	LOG_INF("WDOG0 armed: load %u, ctrl 0x%x", data->load, ctrl);
	return 0;
}

static int agm_wdt_disable(const struct device *dev)
{
	const struct agm_wdt_cfg *cfg = dev->config;
	struct agm_wdt_data *data = dev->data;

	if (!data->setup_done) {
		return -EFAULT;
	}

	/* Reload the counter first so a pending timeout cannot reset
	 * the SoC mid-disable, then clear INTEN/RESEN. */
	agm_wdt_feed_hw(cfg);
	agm_wdt_unlock(cfg);
	agm_wdt_write(cfg, AGM_WDOG_CTRL, 0U);
	agm_wdt_lock(cfg);

	data->setup_done = false;
	return 0;
}

static int agm_wdt_feed(const struct device *dev, int channel_id)
{
	const struct agm_wdt_cfg *cfg = dev->config;
	struct agm_wdt_data *data = dev->data;

	if (channel_id != 0 || !data->setup_done) {
		return -EINVAL;
	}

	/* Clearing the interrupt (WdogIntClr) also reloads the counter
	 * (SDK WDOG_Feed). If the source was masked by an earlier
	 * unserviced timeout, re-enable it so future timeouts interrupt
	 * again. */
	agm_wdt_feed_hw(cfg);
	if (data->irq_masked) {
		irq_enable(cfg->irq);
		data->irq_masked = false;
	}
	return 0;
}

/* Zephyr's ISR type, like the UART and DMA drivers' (see uart_agm.c for the
 * full reason): IRQ_CONNECT() pastes __isr_ ## <handler> to build the table
 * symbol, so the handler has to be a bare identifier and cannot be cast at the
 * call site -- and the native/POSIX arch's ARCH_IRQ_CONNECT() is the only one
 * that does not cast it itself, which makes a `const struct device *` handler
 * -Wincompatible-pointer-types there. */
static void agm_wdt_isr(const void *isr_arg)
{
	const struct device *dev = isr_arg;
	const struct agm_wdt_cfg *cfg = dev->config;
	struct agm_wdt_data *data = dev->data;
	wdt_callback_t cb = NULL;

	if ((agm_wdt_read(cfg, AGM_WDOG_MIS) & AGM_WDOG_INT_MSK) == 0U) {
		return;
	}

	/* Mask the source first: the interrupt stays pending (level) until
	 * wdt_feed() clears it, and an unmasked pending source would
	 * re-enter this ISR in a tight loop. wdt_feed() re-enables it.
	 *
	 * No callback: leave the timeout unserviced on purpose - with
	 * RESEN set the SoC resets when the counter reaches zero again
	 * (SDK "fail after N feeds" model). With a callback: hand over;
	 * the callback feeds (wdt_feed) to survive, or the SoC resets on
	 * the next un-fed timeout. */
	irq_disable(cfg->irq);
	data->irq_masked = true;

	cb = data->callback;
	if (cb != NULL) {
		cb(dev, 0);
	}
}

static int agm_wdt_init(const struct device *dev)
{
	const struct agm_wdt_cfg *cfg = dev->config;

	/* WDOG0's APB gate (agm,apb-clkenable-bit = bit 1) was opened by soc.c
	 * at PRE_KERNEL_1 from devicetree. The separate SYS.APB_CLKSTOP bit
	 * used by WDT_OPT_PAUSE_HALTED_BY_DBG stays driver-side. */

	/* Start from a known disabled state (control resets to 0, but
	 * be explicit and also clear any stale pending interrupt). */
	agm_wdt_unlock(cfg);
	agm_wdt_write(cfg, AGM_WDOG_CTRL, 0U);
	agm_wdt_write(cfg, AGM_WDOG_INTCLR, 1U);
	agm_wdt_lock(cfg);

	cfg->irq_config();
	LOG_INF("AGM WDOG0 initialized (pclk %u Hz)", cfg->pclk_hz);
	return 0;
}

static DEVICE_API(wdt, agm_wdt_api) = {
	.setup = agm_wdt_setup,
	.disable = agm_wdt_disable,
	.install_timeout = agm_wdt_install_timeout,
	.feed = agm_wdt_feed,
};

#define AGM_WDT_IRQ_CONNECT(n)						\
	static void agm_wdt_irq_config_##n(void)			\
	{								\
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority),	\
			    agm_wdt_isr, DEVICE_DT_INST_GET(n), 0);	\
		irq_enable(DT_INST_IRQN(n));				\
	}

#define AGM_WDT_INIT(n)							\
	AGM_WDT_IRQ_CONNECT(n)						\
	static const struct agm_wdt_cfg agm_wdt_cfg_##n = {		\
		.base = DT_INST_REG_ADDR(n),				\
		.pclk_hz = DT_INST_PROP_BY_PHANDLE(n, clocks, clock_frequency),		\
		.irq = DT_INST_IRQN(n),					\
		.irq_config = agm_wdt_irq_config_##n,			\
	};								\
	static struct agm_wdt_data agm_wdt_data_##n;			\
	DEVICE_DT_INST_DEFINE(n, agm_wdt_init, NULL, &agm_wdt_data_##n,	\
			      &agm_wdt_cfg_##n, POST_KERNEL,		\
			      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &agm_wdt_api);

DT_INST_FOREACH_STATUS_OKAY(AGM_WDT_INIT)
