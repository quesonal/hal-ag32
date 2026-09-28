/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * PWM driver for the AGM AgRV2K general-purpose timers
 * (GPTIMER0-4). Reuses the same IP as counter_agm_gptimer; the
 * two drivers are mutually exclusive on a given instance.
 *
 * Hardware (SDK gptimer.h, STM32-style):
 *   CR1 0x00 / CR2 0x04 / SMCR 0x08 / DIER 0x0C / SR 0x10 / EGR 0x14
 *   CCMR0/1 0x18/0x1C / CCER 0x20 / CNT 0x24 / PSC 0x28 / ARR 0x2C
 *   RCR 0x30 / CCR0-3 0x34..0x40 / BDTR 0x44
 *
 * v1 scope:
 *   - 4 channels (CC0-CC3) per instance, mapped to PWM1 mode (active
 *     while CNT < CCRx).
 *   - 32-bit period (ARR) and 32-bit pulse (CCRx).
 *   - Polarity via CCER.CCxP (PWM_POLARITY_NORMAL/INVERTED).
 *   - ARR preload enabled; ARR is written via EGR.UG so the next
 *     period picks up the new value (consistent with the counter
 *     driver's EGR.UG use).
 *   - BDTR.MOE toggled on at init so any pin routed to OCx by the
 *     bitstream actually drives.
 *
 * AF routing: SDK's GPIO_AF_ENABLE sets the GPIOx AFSEL bit for the
 * OCx output (see SDK gpio.h: GPIO_AF_ENABLE_(__PIN) -> sets
 * GPIOx(__PIN##_AF_GPIO)->GpioAFSEL |= __PIN##_AF_GPIO_MASK). The
 * mapping is (from SDK AltaRiscv.h):
 *   GPTIMER0_CH0..3 -> GPIO1 bits 0..3
 *   GPTIMER1_CH0..3 -> GPIO1 bits 4..7
 *   GPTIMER2_CH0..3 -> GPIO2 bits 0..3
 *   GPTIMER3_CH0..3 -> GPIO2 bits 4..7
 *   GPTIMER4_CH0..3 -> GPIO3 bits 0..3
 * Without AFSEL set, the OCx signal never reaches the L100 fabric
 * even if CCER.CCxE and BDTR.MOE are correct. This driver sets
 * AFSEL on every pwm_set_cycles call (matching the SDK's per-pin
 * GPIO_AF_ENABLE pattern).
 *
 * Not implemented:
 *   - Complementary outputs (CHxN), dead-time, break input (BDTR
 *     DTG/BKE fields).
 *   - Channel synchronization (CCPC bit in CR2).
 *   - ETR external trigger, with its SMCR.ETPS trigger prescaler
 *     (SDK GPTIMER_ETR_PrescalerTypeDef, /1../8).
 *
 * Prescaler: PSC (0x28) comes from the node's `prescaler` property
 * (default 0) and is programmed at init, before the EGR.UG that latches
 * it. The tick rate is pclk / (prescaler + 1), and
 * pwm_get_cycles_per_sec() reports that divided value.
 *
 * On-target verification (register-level, no bitstream dependency):
 *   1. driver binds, device_is_ready
 *   2. pwm_set_cycles(period, pulse) returns 0
 *   3. readback ARR + CCRx matches; CCER.CCxE bit set
 *   4. SR.CCxIF toggles at the expected rate (sample uses
 *      busy-loop to count compare events per second and prints
 *      measured PWM frequency)
 *
 */

#define DT_DRV_COMPAT agm_agrv2k_gptimer_pwm

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

#include <agm_sys.h>

LOG_MODULE_REGISTER(pwm_agm_gptimer, CONFIG_PWM_LOG_LEVEL);

/* SYS controller APB clock gate + GPIO bank addresses/offsets come from
 * agm_sys.h.
 */

/* GPTIMER register offsets (SDK gptimer.h). */
#define AGMV2K_GPT_CR1		0x00U
#define AGMV2K_GPT_CR2		0x04U
#define AGMV2K_GPT_DIER		0x0cU
#define AGMV2K_GPT_SR		0x10U
#define AGMV2K_GPT_EGR		0x14U
#define AGMV2K_GPT_CCMR0	0x18U
#define AGMV2K_GPT_CCMR1	0x1cU
#define AGMV2K_GPT_CCER		0x20U
#define AGMV2K_GPT_CNT		0x24U
#define AGMV2K_GPT_PSC		0x28U
#define AGMV2K_GPT_ARR		0x2cU
#define AGMV2K_GPT_CCR0		0x34U
#define AGMV2K_GPT_BDTR		0x44U

#define AGMV2K_GPT_CCRX(ch)	(AGMV2K_GPT_CCR0 + ((ch) * 4U))

/* CR1 */
#define AGMV2K_GPT_CR1_CEN	BIT(0)
#define AGMV2K_GPT_CR1_ARPE	BIT(7)

/* CCMR0/1 — channels 0/1 in CCMR0, channels 2/3 in CCMR1.
 * Each channel occupies 8 bits: OCxM[2:0] in [6:4], OCxPE in 3.
 *   0b110 = PWM1 (active while CNT < CCRx)
 *   0b111 = PWM2 (active while CNT > CCRx) — not used in v1
 */
#define AGMV2K_GPT_OCM_PWM1	(6U << 4)
#define AGMV2K_GPT_OCM_MASK	(7U << 4)
#define AGMV2K_GPT_OCPE		BIT(3)

/* CCER — 4 bits per channel: CCxNP[3] CCxNE[2] CCxP[1] CCxE[0]. */
#define AGMV2K_GPT_CCER_CC1E	0x0001U
#define AGMV2K_GPT_CCER_CC1P	0x0002U
#define AGMV2K_GPT_CCER_CC2E	0x0010U
#define AGMV2K_GPT_CCER_CC2P	0x0020U
#define AGMV2K_GPT_CCER_CC3E	0x0100U
#define AGMV2K_GPT_CCER_CC4E	0x1000U
#define AGMV2K_GPT_CCER_CCxE(ch)	(1U << ((ch) * 4U))
#define AGMV2K_GPT_CCER_CCxP(ch)	(1U << ((ch) * 4U + 1U))

/* BDTR */
#define AGMV2K_GPT_BDTR_MOE	BIT(15)

/* EGR */
#define AGMV2K_GPT_EGR_UG	BIT(0)

#define AGMV2K_GPT_CHANNELS	4U

struct pwm_agm_gptimer_config {
	volatile uint32_t *base;
	uint32_t pclk_hz;
	uint32_t prescaler;
	/* CH0..3 pins, from the node's pinctrl state (gptN_pwm_default):
	 * AGM_PINCTRL() cells applied by pinctrl_configure_pins(). */
	const struct pinctrl_dev_config *pincfg;
};

static inline uint32_t pwm_read(const struct pwm_agm_gptimer_config *cfg,
				uint32_t off)
{
	return sys_read32((mem_addr_t)cfg->base + off);
}

static inline void pwm_write(const struct pwm_agm_gptimer_config *cfg,
			     uint32_t off, uint32_t val)
{
	sys_write32(val, (mem_addr_t)cfg->base + off);
}

static inline void pwm_set_bits(const struct pwm_agm_gptimer_config *cfg,
				uint32_t off, uint32_t mask)
{
	pwm_write(cfg, off, pwm_read(cfg, off) | mask);
}

static inline void pwm_clear_bits(const struct pwm_agm_gptimer_config *cfg,
				  uint32_t off, uint32_t mask)
{
	pwm_write(cfg, off, pwm_read(cfg, off) & ~mask);
}

static int pwm_agm_gptimer_set_cycles(const struct device *dev,
				      uint32_t channel,
				      uint32_t period_cycles,
				      uint32_t pulse_cycles,
				      pwm_flags_t flags)
{
	const struct pwm_agm_gptimer_config *cfg = dev->config;
	uint32_t ccmrx;
	uint32_t ccer_field_e;
	uint32_t ccer_field_p;

	if (channel >= AGMV2K_GPT_CHANNELS) {
		LOG_ERR("invalid channel %u (max %u)", channel,
			AGMV2K_GPT_CHANNELS - 1U);
		return -EINVAL;
	}

	/* Build the CCER bit masks only now that `channel` is known to be in
	 * range: AGMV2K_GPT_CCER_CCxE()/CCxP() shift by channel * 4, so a
	 * rejected channel >= 8 would be a >= 32-bit shift on uint32_t, i.e.
	 * undefined behaviour -- and the compiler is free to exploit it. */
	ccer_field_e = AGMV2K_GPT_CCER_CCxE(channel);
	ccer_field_p = AGMV2K_GPT_CCER_CCxP(channel);

	/* Disable output before reprogramming, to avoid a glitch on
	 * the pin (matches the sf32lb_gpt pattern).
	 */
	pwm_clear_bits(cfg, AGMV2K_GPT_CCER, ccer_field_e);

	/*
	 * Contract of the two zero cases (the Zephyr PWM API
	 * does not spell them out for this driver):
	 *   period_cycles == 0  -> disable *this* channel and leave it
	 *                          disabled; the counter and the other
	 *                          channels keep running (the caller is
	 *                          expected to have used pwm_set_cycles() with
	 *                          a real period to re-enable it).
	 *   period_cycles == 1  -> -EINVAL: not representable, see below.
	 *   pulse_cycles  == 0  -> 0 % duty: the channel stays enabled and
	 *                          drives the pin continuously low.
	 * Anything else is the usual ARR/CCRx programming below. A period of 0
	 * with a non-zero pulse is therefore "off", not "100 %".
	 */
	if (period_cycles == 0U) {
		/* Disabled above (CCER.CCxE cleared), counter untouched. */
		return 0;
	}

	/* ARR is the period; we use 0..ARR inclusive, so the value loaded is
	 * (period - 1). A period of 1 would load ARR = 0, and on this part a
	 * zero auto-reload stops the whole counter rather than just this
	 * channel, so the shortest representable period is 2 ticks. ARR and
	 * CCRx are otherwise 32-bit, with no upper bound to enforce (the old
	 * `period_cycles > UINT32_MAX` test could never fire).
	 *
	 * Pulse is the high-time in counter ticks (CCRx value).
	 * A pulse longer than the period is refused, not clamped: Zephyr's
	 * z_impl_pwm_set_cycles() already answers -EINVAL for it, every upstream
	 * PWM driver that checks does the same (pwm_ameba, pwm_bitbang,
	 * pwm_nxp_flexio, ...), and the clamp this replaced made the same call
	 * mean "100 % duty" when it arrived through the api table instead of
	 * through the API (see below). `pulse == period` stays legal and
	 * *is* 100 %: CCRx = period > ARR = period - 1, so `CNT < CCRx` always
	 * holds and the channel stays active for the whole period. */
	if (period_cycles < 2U) {
		LOG_ERR("period %u below the 2-tick minimum", period_cycles);
		return -EINVAL;
	}

	if (pulse_cycles > period_cycles) {
		LOG_ERR("pulse %u exceeds period %u", pulse_cycles, period_cycles);
		return -EINVAL;
	}

	/* CCMR0 holds channels 0+1, CCMR1 holds 2+3. Each channel
	 * uses the upper 8 bits of the 16-bit register pair's "OCx"
	 * field; here we use the high byte for each (matching the
	 * STM32 datasheet convention: OCxM in bits [6:4] of the
	 * 8-bit slice). Set PWM1 + preload.
	 */
	if (channel < 2U) {
		ccmrx = AGMV2K_GPT_CCMR0;
	} else {
		ccmrx = AGMV2K_GPT_CCMR1;
	}
	uint32_t shift = ((channel & 1U) ? 8U : 0U);

	pwm_clear_bits(cfg, ccmrx,
		       (AGMV2K_GPT_OCM_MASK | AGMV2K_GPT_OCPE) << shift);
	pwm_set_bits(cfg, ccmrx,
		     (AGMV2K_GPT_OCM_PWM1 | AGMV2K_GPT_OCPE) << shift);

	/* Polarity. Clear first, then set if inverted. */
	pwm_clear_bits(cfg, AGMV2K_GPT_CCER, ccer_field_p);
	if ((flags & PWM_POLARITY_MASK) == PWM_POLARITY_INVERTED) {
		pwm_set_bits(cfg, AGMV2K_GPT_CCER, ccer_field_p);
	}

	/* Period and pulse. EGR.UG after writes so the new ARR is
	 * picked up immediately rather than after the next update
	 * event. (ARR preload bit is set in CCMRx above, so without
	 * UG the new ARR waits for the next CNT wrap to load.)
	 */
	pwm_write(cfg, AGMV2K_GPT_ARR, period_cycles - 1U);
	pwm_write(cfg, AGMV2K_GPT_CCRX(channel), pulse_cycles);

	pwm_write(cfg, AGMV2K_GPT_EGR, AGMV2K_GPT_EGR_UG);

	/* Re-enable the channel output. */
	pwm_set_bits(cfg, AGMV2K_GPT_CCER, ccer_field_e);

	return 0;
}

static int pwm_agm_gptimer_get_cycles_per_sec(const struct device *dev,
					      uint32_t channel,
					      uint64_t *cycles)
{
	const struct pwm_agm_gptimer_config *cfg = dev->config;

	if (channel >= AGMV2K_GPT_CHANNELS) {
		return -EINVAL;
	}
	/* The counter ticks at pclk / (PSC + 1); the prescaler comes
	 * from the node and defaults to 0 (divide by one).
	 */
	*cycles = cfg->pclk_hz / (cfg->prescaler + 1U);
	return 0;
}

/* DEVICE_API(), like every other driver in this tree and every upstream PWM
 * driver: it places the table in the class's API section, which is what
 * DEVICE_API_GET() checks (CONFIG_DEVICE_API_ASSERT, default y under
 * CONFIG_ASSERT). With a plain `static const struct pwm_driver_api` the check
 * fails, and the PWM API -- pwm_set_cycles() and pwm_get_cycles_per_sec() both
 * go through DEVICE_API_GET() -- panics with "device API is not pwm"
 * (found while adding the native suite: the panic is the first thing that
 * happens in any CONFIG_ASSERT build, i.e. in every ztest/native config). */
static DEVICE_API(pwm, pwm_agm_gptimer_api) = {
	.set_cycles = pwm_agm_gptimer_set_cycles,
	.get_cycles_per_sec = pwm_agm_gptimer_get_cycles_per_sec,
};

static int pwm_agm_gptimer_init(const struct device *dev)
{
	const struct pwm_agm_gptimer_config *cfg = dev->config;
	int err;

	/* Route CH0..3 to their pins. The state lists one AGM_PINCTRL() cell
	 * per channel (NO_DIR: the timer drives the pin, we only own AFSEL),
	 * and soc.c opened both the instance gate and the pins' bank gate at
	 * PRE_KERNEL_1 from the same state. */
	err = pinctrl_apply_state(cfg->pincfg, PINCTRL_STATE_DEFAULT);
	if (err != 0) {
		LOG_ERR("pinctrl_apply_state failed: %d", err);
		return err;
	}

	/* Set up the counter in up-counting mode, ARR preload on.
	 * BDTR.MOE enables the OCx outputs to the fabric (without
	 * this the pin never drives even if CCER.CCxE is set).
	 *
	 * PSC has to be written before the EGR.UG below, which is what
	 * latches it into the running counter.
	 */
	pwm_write(cfg, AGMV2K_GPT_PSC, cfg->prescaler);
	pwm_set_bits(cfg, AGMV2K_GPT_CR1, AGMV2K_GPT_CR1_ARPE);
	pwm_set_bits(cfg, AGMV2K_GPT_BDTR, AGMV2K_GPT_BDTR_MOE);
	pwm_set_bits(cfg, AGMV2K_GPT_CR1, AGMV2K_GPT_CR1_CEN);

	/* Generate an update event so the ARR/CCRx preloads (and the
	 * prescaler) latch on init, matching SDK GPTIMER_Init() which
	 * calls GPTIMER_GenerateEventUpdate() at the end. Harmless
	 * since CCER.CCxE is still 0 - no output is driven yet.
	 */
	pwm_write(cfg, AGMV2K_GPT_EGR, AGMV2K_GPT_EGR_UG);

	return 0;
}

/* The AFSEL route below derives the timer index from the reg address, so
 * the node must sit on one of the GPTIMER0..4 pages. Do NOT compare
 * against AGM_GPTIMER_BASE(n): Zephyr renumbers DT instances so
 * status-okay nodes come first, so the instance number says nothing about
 * the dtsi position.
 */
#define PWM_AGM_GPTIMER_INIT(n)						\
	BUILD_ASSERT(((DT_INST_REG_ADDR(n) - AGM_GPTIMER_BASE(0)) % AGM_APB_STRIDE) == 0 && \
		     ((DT_INST_REG_ADDR(n) - AGM_GPTIMER_BASE(0)) / AGM_APB_STRIDE) < 5U, \
		     "gptimer" #n " reg address is not one of the GPTIMER0..4 pages"); \
	PINCTRL_DT_INST_DEFINE(n);					\
	static const struct pwm_agm_gptimer_config			\
		pwm_agm_gptimer_cfg_##n = {				\
		.base = (volatile uint32_t *)				\
			DT_INST_REG_ADDR(n),				\
		.pclk_hz = DT_INST_PROP_BY_PHANDLE(n, clocks, clock_frequency),		\
		.prescaler = DT_INST_PROP(n, prescaler),				\
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),		\
	};								\
									\
	DEVICE_DT_INST_DEFINE(n,					\
			      pwm_agm_gptimer_init,			\
			      NULL,					\
			      NULL,					\
			      &pwm_agm_gptimer_cfg_##n,			\
			      POST_KERNEL,				\
			      CONFIG_PWM_INIT_PRIORITY,			\
			      &pwm_agm_gptimer_api);

DT_INST_FOREACH_STATUS_OKAY(PWM_AGM_GPTIMER_INIT)
