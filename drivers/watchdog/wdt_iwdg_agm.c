/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AGM AgRV2K IWDG driver — backup-domain independent watchdog
 * (RTC->IWDG @ 0x40000034).
 *
 * Hardware (vendor SDK framework-agrv_sdk/src/iwdg.h + rtc.h):
 *
 *   Register 16-bit at 0x40000034 in the RTC / backup domain.
 *   No APB clock gate and no PLIC IRQ — the only reaction to a
 *   timeout is a whole-SoC reset. All writes to the backup domain
 *   must wait for RTC->CRL.RTOFF (the SDK calls this
 *   RTC_WaitForWrite()).
 *
 *   IWDG register layout:
 *     bit 0..2  prescaler (3-bit divisor: 0=/2, 1=/4, ..., 7=/256)
 *     bit 4     IWDG_STOP_FREEZE   — freeze in STOP mode
 *     bit 5     IWDG_STANDBY_FREEZE — freeze in STANDBY mode
 *     bit 6     IWDG_CLKSEL        — 0 = LSI, 1 = LSE
 *     bit 8     IWDG_ENABLE
 *
 *   "Feed" writes the 16-bit key 0xa000 to IWDG. There is no
 *   user-readable reload value: the vendor-fixed reload count
 *   determines the timeout together with the prescaler and the
 *   source clock (LSI nominal 32 kHz, LSE 32768 Hz).
 *
 * Zephyr WDT API notes:
 *
 *   - callback is rejected (-ENOTSUP): no IRQ path.
 *   - window.min must be 0: this is not a windowed watchdog.
 *   - reset_flags must be WDT_FLAG_RESET_SOC: reset-only.
 *   - WDT_OPT_PAUSE_HALTED_BY_DBG returns -ENOTSUP: there is no
 *     debug-halt clock stop in the backup domain.
 *   - WDT_OPT_PAUSE_IN_SLEEP is implemented by setting
 *     IWDG_STOP_FREEZE; the freeze-mode DT property selects the
 *     broader policy (which low-power modes freeze the counter).
 *
 * Disable semantics: writing IWDG_ENABLE = 0 stops the counter but
 * leaves the rest of the backup-domain state intact. A subsequent
 * enable re-uses the same prescaler / clock source / freeze
 * configuration; the vendor SDK reaches for a hard reset only via
 * RTC->BDRST (whole backup-domain pulse), which is not exposed
 * through this driver.
 */

#define DT_DRV_COMPAT agm_agrv2k_iwdg

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/counter/agm_rtc_regs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(wdt_iwdg_agm, CONFIG_WDT_LOG_LEVEL);

/* Bounded LSE start-up wait. Mirrors counter_agm_rtc.c's AGM_RTC_LSE_ROUNDS:
 * the crystal needs seconds after a backup-domain reset, an absent crystal
 * needs to fail instead of hanging wdt_setup() forever. One spin is a register
 * poll plus a loop branch, so ~2M of them are ~1 s on this core. */
#define AGM_IWDG_LSE_SPINS   2000000U
#define AGM_IWDG_LSE_ROUNDS  4U

/* RTC base (also see drivers/counter/counter_agm_rtc.c). The register offsets
 * and IWDG bits come from the shared map in
 * include/zephyr/drivers/counter/agm_rtc_regs.h -- one copy, because the
 * bootloader and the applications it boots touch the same registers through
 * <zephyr/drivers/misc/boot_agm.h>. */
#define AGM_RTC_BASE          0x40000000U

/* The IWDG lives in the same backup domain as the RTC; vendor SDK
 * rtc.c::RTC_Init() sets BDCR.RTCEN and the RTCSEL field before any IWDG write
 * can latch, and rtc0 is status="disabled" by default, so this driver has to
 * enable the backup domain itself: the latch is not armed
 * until BDRST has been pulsed at least once: without it the IWDG register reads
 * back as 0x0000 even after a correct write. */

/* The IWDG register bits under their shared names. */
#define AGM_IWDG_REG_OFFSET   AGM_RTC_IWDG
#define IWDG_PRESCALER_MASK   AGM_RTC_IWDG_PRESCALER_MASK
#define IWDG_STOP_FREEZE      AGM_RTC_IWDG_STOP_FREEZE
#define IWDG_STANDBY_FREEZE   AGM_RTC_IWDG_STANDBY_FREEZE
#define IWDG_FREEZE_MASK      (IWDG_STOP_FREEZE | IWDG_STANDBY_FREEZE)
#define IWDG_CLKSEL           AGM_RTC_IWDG_CLKSEL
#define IWDG_ENABLE           AGM_RTC_IWDG_ENABLE
#define IWDG_RELOAD_MASK      AGM_RTC_IWDG_RELOAD_MASK
#define IWDG_RELOAD_KEY       AGM_RTC_IWDG_RELOAD_KEY

/*
 * Vendor-fixed reload count. The SDK does not expose a register to
 * read or write this; the only way to keep the IWDG alive is to feed
 * it. With the nominal LSI = 32 kHz and prescaler /64 the timeout is
 * roughly 64 × vendor_reload / 32000 s. We treat the load as opaque
 * and just expose the discrete prescaler taps to the user.
 *
 * The mapping table below uses LSI = 32 kHz (datasheet nominal). Real
 * LSI drifts with temperature (~±30 %); LSE = 32768 Hz is exact. The
 * timeout error is the user's problem — wdt_agm has the same caveat
 * for the WDOG0 path.
 */
static const struct {
	uint16_t div;       /* prescaler divisor (uint16: /256 = 0x100
			     * does not fit in uint8). */
	uint8_t reg;        /* value to write into the 3-bit prescaler field */
	uint32_t timeout_ms; /* nominal, LSI = 32 kHz */
} prescaler_lut[] = {
	{   2U, 0U,     64U },   /* /2  : 64 ms */
	{   4U, 1U,    128U },   /* /4  : 128 ms */
	{   8U, 2U,    256U },   /* /8  : 256 ms */
	{  16U, 3U,    512U },   /* /16 : 512 ms */
	{  32U, 4U,   1024U },   /* /32 : ~1 s */
	{  64U, 5U,   2048U },   /* /64 : ~2 s */
	{ 128U, 6U,   4096U },   /* /128: ~4 s */
	{ 256U, 7U,   8192U },   /* /256: ~8 s */
};
#define AGM_IWDG_PRESCALER_COUNT ARRAY_SIZE(prescaler_lut)

struct agm_iwdg_cfg {
	uint16_t prescaler_reg;  /* value written into the 3-bit field */
	uint16_t clksel_bit;     /* IWDG_CLKSEL or 0 */
	uint16_t freeze_mask;    /* IWDG_STOP_FREEZE | IWDG_STANDBY_FREEZE */
};

static struct agm_iwdg_cfg agm_iwdg_cfg_0;  /* filled by agm_iwdg_init() */

struct agm_iwdg_data {
	bool setup_done;
	/* Timeout the last install_timeout() asked for, 0 when nobody asked.
	 * The register only has the eight prescaler taps, so setup() rounds
	 * this up to the next tap -- see pick_prescaler_index(). */
	uint32_t requested_ms;
};

/*
 * 16-bit backup-domain register access.
 *
 * The RTC/IWDG registers here are half-word wide and Zephyr does not export
 * sys_read16/sys_write16, so the window is touched through these two helpers --
 * one place that knows how, like counter_agm_rtc.c's accessors (the driver
 * used to open-code the volatile pointer at every site).
 */
static inline uint16_t bkp_read16(uint32_t off)
{
	return *(volatile uint16_t *)(AGM_RTC_BASE + off);
}

static inline void bkp_write16(uint32_t off, uint16_t val)
{
	*(volatile uint16_t *)(AGM_RTC_BASE + off) = val;
}

static inline void rtc_wait_write(void)
{
	/* Poll RTC->CRL.RTOFF: vendor SDK rtc.h: "Any write to
	 * RTC/backup domain register need to wait for RTOFF,
	 * including PRLH/PRLL, ..., IWDG, RTCCR, and BKP_DR".
	 * The read goes through bkp_read16() above. */
	while ((bkp_read16(AGM_RTC_CRL) & AGM_RTC_CRL_RTOFF) == 0U) {
		/* spin */
	}
}

static inline void iwdg_write(uint16_t value)
{
	bkp_write16(AGM_IWDG_REG_OFFSET, value);
	rtc_wait_write();
}

/* Vendor SDK iwdg.h only ever touches the IWDG register via
 * MODIFY_BIT (read-modify-write). iwdg.h is the only SDK header that
 * uses this pattern -- the RTC, GPIO, USART, etc. drivers all do
 * bare writes. Treat it as a hint that the IWDG register needs an
 * explicit "read-back what we are about to overwrite, then write"
 * sequence: a bare write may be silently dropped on this IP. */
static inline void iwdg_modify_bit(uint16_t clearmask, uint16_t setmask)
{
	uint16_t r;

	r = bkp_read16(AGM_IWDG_REG_OFFSET);
	r = (uint16_t)((r & ~(uint16_t)clearmask) | setmask);
	bkp_write16(AGM_IWDG_REG_OFFSET, r);
	rtc_wait_write();
}

static inline uint16_t iwdg_read(void)
{
	return bkp_read16(AGM_IWDG_REG_OFFSET);
}

/* Pick the smallest prescaler index whose nominal timeout (LSI = 32 kHz) is
 * ≥ window_ms. There is no reload register to program, so the eight taps are
 * the whole resolution the IP has: a request of 5 s lands on the /256 tap
 * (8192 ms nominal, and ±30 % with the LSI's temperature drift, so the real
 * timeout can be anywhere around 5.7-10.6 s).
 *
 * With this part's 40 kHz LSI the numbers are ~20 % shorter; the driver
 * keeps the SDK's 32 kHz nominal in the table, so a
 * timeout is always at least as long as asked for. */
static size_t pick_prescaler_index(uint32_t window_ms)
{
	for (size_t i = 0U; i < AGM_IWDG_PRESCALER_COUNT; i++) {
		if (prescaler_lut[i].timeout_ms >= window_ms) {
			return i;
		}
	}
	return AGM_IWDG_PRESCALER_COUNT - 1U;
}

static int agm_iwdg_install_timeout(const struct device *dev,
				    const struct wdt_timeout_cfg *cfg)
{
	struct agm_iwdg_data *data = dev->data;

	if (cfg == NULL || cfg->window.max == 0U) {
		return -EINVAL;
	}
	if (data->setup_done) {
		return -EBUSY;
	}
	if (cfg->callback != NULL) {
		/* No IRQ path; the SDK only resets on timeout. */
		return -ENOTSUP;
	}
	if (cfg->window.min != 0U) {
		/* IWDG is not a windowed watchdog. */
		return -ENOTSUP;
	}

	const uint32_t reset_flags = cfg->flags & WDT_FLAG_RESET_MASK;

	if (reset_flags != WDT_FLAG_RESET_SOC) {
		/* Reset-only IP; WDT_FLAG_RESET_NONE / _CPU_CORE both
		 * leave the SoC running but the IWDG cannot notify
		 * anything — neither makes sense. */
		return -ENOTSUP;
	}
	if ((cfg->flags & ~WDT_FLAG_RESET_MASK) != 0U) {
		return -ENOTSUP;
	}

	/* Bounds check: the smallest timeout is 64 ms (prescaler /2). */
	if (cfg->window.max < prescaler_lut[0].timeout_ms) {
		return -EINVAL;
	}
	/* ... and the largest is 8192 ms (/256). Silently arming something
	 * much shorter than asked for would be worse than refusing. */
	if (cfg->window.max > prescaler_lut[AGM_IWDG_PRESCALER_COUNT - 1U].timeout_ms) {
		return -EINVAL;
	}

	data->requested_ms = cfg->window.max;
	return 0; /* channel 0 */
}

static int agm_iwdg_setup(const struct device *dev, uint8_t options)
{
	const struct agm_iwdg_cfg *cfg = dev->config;
	struct agm_iwdg_data *data = dev->data;
	uint16_t prescaler_reg = cfg->prescaler_reg;
	uint16_t bdcr;

	if (data->setup_done) {
		return -EBUSY;
	}
	if (options & WDT_OPT_PAUSE_HALTED_BY_DBG) {
		/* No debug-halt clock stop in the backup domain. */
		return -ENOTSUP;
	}

	(void)options; /* WDT_OPT_PAUSE_IN_SLEEP maps to STOP_FREEZE in
			* the freeze-mode DT property, not options. */

	/* A requested timeout wins over the build-time prescaler: that is what
	 * install_timeout() promises, and the DT value stays the default for
	 * callers who arm the watchdog without asking for a window. */
	if (data->requested_ms != 0U) {
		prescaler_reg = prescaler_lut[pick_prescaler_index(data->requested_ms)].reg;
	}

	/* Enable the backup domain and select the IWDG clock source.
	 * vendor SDK rtc.c::RTC_Init() sets BDCR.RTCEN=1 + RTCSEL
	 * (bits 9:8) before any IWDG write can latch; rtc0 is
	 * status="disabled" so its driver doesn't run, leaving
	 * BDCR.RTCEN=0 and silently dropping every IWDG write (the
	 * register readback stays 0x0000). Doing this here also matches
	 * the SDK rtc.h invariant "Any write to RTC/backup domain
	 * register need to wait for RTOFF".
	 *
	 * The SDK also calls RTC_Reset() (BDRST bit pulse + CRH/CRL=0)
	 * before RTC_Init(). Without the BDRST pulse the IWDG register
	 * reads back as 0x0000 even after BDCR.RTCEN
	 * is set and a correct write: the backup-domain latch is not
	 * armed until BDRST has been toggled at least once on a fresh
	 * boot. Do the same here, then re-write BDCR.
	 *
	 * RTCSEL: 2 = LSI (40 kHz internal, 32 kHz per SDK nominal),
	 * 1 = LSE (32768 Hz).
	 * LSE additionally needs BDCR.LSEON=1 + wait for BDCR.LSERDY;
	 * if the board has no 32768 Hz crystal the spin will hang, so
	 * agm,clk-source = "lsi" is the safe default in dtsi.
	 *
	 * BDCR.RTCSEL is write-once until a backup-domain reset, so if the
	 * domain is already enabled somebody else owns that field -- normally
	 * counter_agm_rtc.c -- and neither the BDRST pulse below (which would
	 * wipe that driver's configuration) nor a second RTCSEL write (which
	 * the hardware ignores) belongs here. Read back what is actually in
	 * effect instead of assuming our value took. */
	bdcr = bkp_read16(AGM_RTC_BDCR);

	if ((bdcr & AGM_RTC_BDCR_RTCEN) != 0U) {
		bool effective_lse =
			(((bdcr >> AGM_RTC_BDCR_RTCSEL_OFF) & 0x3U) == 1U);

		if (effective_lse != ((cfg->clksel_bit & IWDG_CLKSEL) != 0U)) {
			LOG_WRN("backup-domain clock is %s (another driver owns "
				"BDCR.RTCSEL); the IWDG runs from its own CLKSEL",
				effective_lse ? "LSE" : "LSI");
		}
	} else {
		bkp_write16(AGM_RTC_BDRST, AGM_RTC_BDRST_BIT);
		rtc_wait_write();
		bkp_write16(AGM_RTC_BDRST, 0U);
		rtc_wait_write();
		/* Zero CRH/CRL so RSF/SEC/ALR/OW flags do not block writes. */
		bkp_write16(AGM_RTC_CRL, 0U);
		rtc_wait_write();

		if (cfg->clksel_bit & IWDG_CLKSEL) {
			/* LSE: turn on the oscillator and wait for it. */
			bdcr = AGM_RTC_BDCR_RTCEN | (1U << AGM_RTC_BDCR_RTCSEL_OFF) |
			       AGM_RTC_BDCR_LSEON;
		} else {
			bdcr = AGM_RTC_BDCR_RTCEN | (2U << AGM_RTC_BDCR_RTCSEL_OFF);
		}
		bkp_write16(AGM_RTC_BDCR, bdcr);
		rtc_wait_write();
	}

	if (cfg->clksel_bit & IWDG_CLKSEL) {
		/* The IWDG's own CLKSEL bit selects LSE, which needs the
		 * oscillator running: assert LSEON *keeping* whatever RTCSEL /
		 * RTCEN are in effect (LSEON is not write-once, RTCSEL is), then
		 * wait for the crystal in rounds, re-asserting LSEON between them
		 * (the same shape counter_agm_rtc.c uses). A board with no
		 * 32768 Hz crystal must fail here, not hang: wdt_setup() is
		 * called from application code, and a silent spin also means
		 * wdt_feed() never runs. */
		bool ready = false;

		bdcr |= AGM_RTC_BDCR_LSEON;
		bkp_write16(AGM_RTC_BDCR, bdcr);
		rtc_wait_write();

		for (uint32_t round = 0U;
		     (round < AGM_IWDG_LSE_ROUNDS) && !ready; round++) {
			uint32_t spins = AGM_IWDG_LSE_SPINS;

			while ((bkp_read16(AGM_RTC_BDCR) &
				AGM_RTC_BDCR_LSERDY) == 0U) {
				if (--spins == 0U) {
					break;
				}
			}
			ready = (bkp_read16(AGM_RTC_BDCR) &
				 AGM_RTC_BDCR_LSERDY) != 0U;
			if (!ready) {
				LOG_WRN("LSE not ready (LSERDY), retry %u/%u",
					round + 1U, AGM_IWDG_LSE_ROUNDS);
				bkp_write16(AGM_RTC_BDCR, bdcr);
				rtc_wait_write();
			}
		}
		if (!ready) {
			LOG_ERR("LSE not ready (LSERDY) after %u rounds; check the "
				"board crystal or use agm,clk-source = \"lsi\"",
				AGM_IWDG_LSE_ROUNDS);
			return -ETIMEDOUT;
		}
	}

	/* Configure the IWDG via read-modify-write -- see iwdg_modify_bit
	 * for the rationale (vendor SDK iwdg.h only ever modifies this
	 * register; bare writes are silently dropped on this IP). */
	iwdg_modify_bit(IWDG_PRESCALER_MASK, prescaler_reg);
	iwdg_modify_bit(IWDG_FREEZE_MASK, cfg->freeze_mask);
	iwdg_modify_bit(IWDG_CLKSEL, cfg->clksel_bit);
	iwdg_modify_bit(IWDG_ENABLE, IWDG_ENABLE);
	/* The vendor SDK clears any stale pending counter state by
	 * feeding immediately after enabling. */
	iwdg_modify_bit(IWDG_RELOAD_MASK, IWDG_RELOAD_KEY);

	data->setup_done = true;
	return 0;
}

static int agm_iwdg_disable(const struct device *dev)
{
	struct agm_iwdg_data *data = dev->data;

	if (!data->setup_done) {
		return -EFAULT;
	}

	/* Clear IWDG_ENABLE while preserving the rest of the backup-domain
	 * state, again via read-modify-write. */
	iwdg_modify_bit(IWDG_ENABLE, 0U);

	data->setup_done = false;
	return 0;
}

static int agm_iwdg_feed(const struct device *dev, int channel_id)
{
	struct agm_iwdg_data *data = dev->data;

	if (channel_id != 0) {
		return -EINVAL;
	}
	if (!data->setup_done) {
		/* Feeding a disabled IWDG is harmless (write 0xa000 to
		 * the IWDG register; the SDK does not gate on
		 * IWDG_ENABLE), but expose it as an error to mirror the
		 * wdt_agm semantics. */
		return -EINVAL;
	}

	iwdg_modify_bit(IWDG_RELOAD_MASK, IWDG_RELOAD_KEY);
	return 0;
}

static int agm_iwdg_init(const struct device *dev);

/* Translate agm,prescaler (integer divisor) → 3-bit register field.
 * The binding enum pins the value to {2,4,8,16,32,64,128,256}. */
static uint8_t agm_iwdg_prescaler_field_from_dt(uint32_t div)
{
	switch (div) {
	case 2U:   return 0U;
	case 4U:   return 1U;
	case 8U:   return 2U;
	case 16U:  return 3U;
	case 32U:  return 4U;
	case 64U:  return 5U;
	case 128U: return 6U;
	case 256U: return 7U;
	default:
		__ASSERT(false, "agm,prescaler %u not in enum", div);
		return 0U;
	}
}

/* agm,clk-source: 0 = lsi, 1 = lse (binding string enum, the driver
 * uses DT_ENUM_IDX which returns the integer index). */
static uint16_t agm_iwdg_clksel_field_from_dt(uint32_t idx)
{
	__ASSERT(idx == 0U || idx == 1U,
		 "agm,clk-source must be 0 (lsi) or 1 (lse)");
	return (idx == 1U) ? IWDG_CLKSEL : 0U;
}

/* agm,freeze-mode: bitmask of STOP_FREEZE / STANDBY_FREEZE.
 * Token mapping follows the binding enum order: 0=none, 1=stop,
 * 2=standby, 3=all. */
static uint16_t agm_iwdg_freeze_mask_from_dt(uint32_t idx)
{
	uint16_t mask = 0U;

	if (idx == 0U || idx == 2U) {
		mask |= IWDG_STOP_FREEZE;
	}
	if (idx == 1U || idx == 2U) {
		mask |= IWDG_STANDBY_FREEZE;
	}
	return mask;
}

static int agm_iwdg_init(const struct device *dev)
{
	struct agm_iwdg_cfg *cfg = (struct agm_iwdg_cfg *)dev->config;

	/* Resolve DT-derived values here instead of in a static
	 * initializer: -std=c17 forbids function calls in struct
	 * initializers, and DT_ENUM_IDX expands to one. cfg is a
	 * non-const static so the writes are well-defined. */
	cfg->prescaler_reg = agm_iwdg_prescaler_field_from_dt(
		DT_PROP(DT_DRV_INST(0), agm_prescaler));
	cfg->clksel_bit = agm_iwdg_clksel_field_from_dt(
		DT_ENUM_IDX(DT_DRV_INST(0), agm_clk_source));
	cfg->freeze_mask = agm_iwdg_freeze_mask_from_dt(
		DT_ENUM_IDX(DT_DRV_INST(0), agm_freeze_mode));

	/* The IWDG has no APB gate and no interrupt line: the SDK does
	 * not touch any SYS.APB_* bit and there is no
	 * irq_config() to call. Do not touch the IWDG register here:
	 * enabling the IWDG is an explicit board-level decision
	 * (status = "disabled" in the dtsi). The driver only arms it
	 * from agm_iwdg_setup(). */
	return 0;
}

static DEVICE_API(wdt, agm_iwdg_api) = {
	.setup = agm_iwdg_setup,
	.disable = agm_iwdg_disable,
	.install_timeout = agm_iwdg_install_timeout,
	.feed = agm_iwdg_feed,
};

/* Translate agm,prescaler (integer divisor) → 3-bit register field.
 * (defined earlier, before agm_iwdg_init). */

#define AGM_IWDG_INST(n)                                                       \
	static struct agm_iwdg_data agm_iwdg_data_##n;                        \
	DEVICE_DT_INST_DEFINE(n, agm_iwdg_init, NULL,                           \
			      &agm_iwdg_data_##n, &agm_iwdg_cfg_##n,            \
			      POST_KERNEL,                                      \
			      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,             \
			      &agm_iwdg_api);

DT_INST_FOREACH_STATUS_OKAY(AGM_IWDG_INST)
