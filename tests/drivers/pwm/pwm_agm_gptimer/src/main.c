/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief native_sim suite for the GPTIMER PWM driver.
 *
 * The window is RAM (tests/drivers/common/agm_native), which is a complete
 * model of this peripheral: the driver writes CR1/CCMR0/CCER/ARR/CCRx/EGR/BDTR
 * and never polls a status bit, so every assertion here is "what did the driver
 * program", read back through the real register addresses.
 *
 * That is enough to pin the parts of this driver nobody could reach before:
 *  - the CCER/CCMR bit arithmetic per channel (four channels, two registers,
 *    a different shift for each),
 *  - the channel bounds check *before* the masks are built (ordering matters:
 *    AGMV2K_GPT_CCER_CCxE(channel) shifts by channel * 4, so a rejected
 *    channel >= 8 was a >= 32-bit shift on uint32_t -- undefined behaviour, and
 *    the compiler may do anything with it),
 *  - the ARR/CCRx programming and the pulse > period refusal (both through the
 *    API and through the api table, same answer),
 *  - period == 0 meaning "disable this channel", not "stop the timer".
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/ztest.h>

#define PWM_DEV  DEVICE_DT_GET(DT_NODELABEL(gpt0_pwm))

/* Offsets from drivers/pwm/pwm_agm_gptimer.c. */
#define GPT_CR1    0x00U
#define GPT_CCMR0  0x18U
#define GPT_CCMR1  0x1cU
#define GPT_CCER   0x20U
#define GPT_ARR    0x2cU
#define GPT_CCR0   0x34U
#define GPT_BDTR   0x44U

#define GPT_BASE   0x40020000UL

#define CR1_CEN    BIT(0)
#define CR1_ARPE   BIT(7)
#define BDTR_MOE   BIT(15)
#define CCER_CCxE(ch) (1U << ((ch) * 4U))
#define CCER_CCxP(ch) (1U << ((ch) * 4U + 1U))
#define OCM_PWM1   (6U << 4)
#define OCPE       BIT(3)

static uint32_t reg_rd(uint32_t off)
{
	return *(volatile uint32_t *)(GPT_BASE + off);
}

static void reg_wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(GPT_BASE + off) = val;
}

static void suite_reset_fake_regs(void *fixture)
{
	ARG_UNUSED(fixture);

	/* Leave CR1/BDTR as init left them (the suite asserts on that in the
	 * first case) and clear the per-channel state the cases program. */
	reg_wr(GPT_CCER, 0U);
	reg_wr(GPT_CCMR0, 0U);
	reg_wr(GPT_CCMR1, 0U);
	reg_wr(GPT_ARR, 0U);
	for (uint32_t ch = 0U; ch < 4U; ch++) {
		reg_wr(GPT_CCR0 + (ch * 4U), 0U);
	}
}

ZTEST_SUITE(pwm_agm_gptimer, NULL, NULL, suite_reset_fake_regs, NULL, NULL);

/* ---- init ------------------------------------------------------------ */

ZTEST(pwm_agm_gptimer, test_01_init_runs_the_counter_and_opens_the_outputs)
{
	zassert_true(device_is_ready(PWM_DEV), "the PWM device came up against the fake window");

	/* ARR preload on, main output enable on, counter enabled. Without MOE
	 * the OCx outputs never reach the fabric even with CCxE set. */
	zassert_equal(reg_rd(GPT_CR1) & (CR1_ARPE | CR1_CEN), CR1_ARPE | CR1_CEN,
		      "ARR preload + counter enable");
	zassert_equal(reg_rd(GPT_BDTR) & BDTR_MOE, BDTR_MOE, "main output enable");

	/* The pinctrl state is applied at init: the four CH pins are AFSEL'd on
	 * GPIO bank 1, with DIR left alone (NO_DIR -- the timer drives them). */
	zassert_equal(*(volatile uint32_t *)(0x40014000UL + 0x1000UL + 0x420UL) & 0xfU, 0xfU,
		      "CH0..3 handed to the timer (AFSEL bits 0..3 of bank 1)");
}

ZTEST(pwm_agm_gptimer, test_02_cycles_per_sec_is_the_pclk)
{
	uint64_t cycles = 0U;

	zassert_ok(pwm_get_cycles_per_sec(PWM_DEV, 0U, &cycles));
	zassert_equal(cycles, 200000000U, "the counter ticks at pclk (PSC is fixed at 0)");

	zassert_equal(pwm_get_cycles_per_sec(PWM_DEV, 4U, &cycles), -EINVAL,
		      "channel 4 does not exist");
}

/* ---- the bounds check has to come first -------------------------------- */

/* The regression: the CCER masks were built from `channel` before the range
 * check, so channel 8 (and anything above) shifted a uint32_t by >= 32 -- UB,
 * on a value the driver had already decided to reject. The observable half is
 * that the call is refused *and* nothing was programmed: with the old order the
 * shift had already happened (and on this compiler produced a mask for another
 * channel) before the -EINVAL. */
ZTEST(pwm_agm_gptimer, test_10_out_of_range_channel_is_refused_before_any_mask)
{
	zassert_equal(reg_rd(GPT_CCER), 0U, "precondition: no channel enabled");

	for (uint32_t ch = 4U; ch < 12U; ch++) {
		zassert_equal(pwm_set_cycles(PWM_DEV, ch, 1000U, 500U, PWM_POLARITY_NORMAL), -EINVAL,
			      "channel %u is out of range", ch);
	}

	zassert_equal(reg_rd(GPT_CCER), 0U, "a refused channel must not touch CCER");
	zassert_equal(reg_rd(GPT_ARR), 0U, "nor ARR");
}

/* ---- the per-channel programming ------------------------------------- */

ZTEST(pwm_agm_gptimer, test_20_channel_0_programs_ccmr0_ccer_arr_and_ccr)
{
	zassert_ok(pwm_set_cycles(PWM_DEV, 0U, 1000U, 250U, PWM_POLARITY_NORMAL));

	zassert_equal(reg_rd(GPT_ARR), 999U, "ARR is period - 1 (0..ARR inclusive)");
	zassert_equal(reg_rd(GPT_CCR0), 250U, "CCR0 is the pulse width");
	zassert_equal(reg_rd(GPT_CCMR0) & (OCM_PWM1 | OCPE), OCM_PWM1 | OCPE,
		      "channel 0 is PWM mode 1 with preload, in CCMR0 bits 4..6");
	zassert_equal(reg_rd(GPT_CCER) & CCER_CCxE(0U), CCER_CCxE(0U), "channel 0 output enabled");
	zassert_equal(reg_rd(GPT_CCER) & CCER_CCxP(0U), 0U, "normal polarity");
}

ZTEST(pwm_agm_gptimer, test_21_each_channel_uses_its_own_bits_and_register)
{
	/* Channels 0/1 live in CCMR0, 2/3 in CCMR1, each in a different byte;
	 * CCER gives each channel four bits. Four channels is the whole v1
	 * scope, and this is the only place that shape is checked. */
	zassert_ok(pwm_set_cycles(PWM_DEV, 2U, 4000U, 1000U, PWM_POLARITY_INVERTED));

	zassert_equal(reg_rd(GPT_ARR), 3999U, "ARR follows the last channel written");
	zassert_equal(reg_rd(GPT_CCR0 + (2U * 4U)), 1000U, "channel 2 uses CCR2");
	zassert_equal(reg_rd(GPT_CCMR0), 0U, "and does not touch CCMR0");
	/* CCMR1 carries channels 2 and 3 in its two bytes, first channel first:
	 * channel 2 is the low byte (shift 0), channel 3 the high one (shift 8). */
	zassert_equal(reg_rd(GPT_CCMR1) & OCM_PWM1, OCM_PWM1,
		      "channel 2 is the low byte of CCMR1");
	zassert_equal(reg_rd(GPT_CCMR1) & (OCM_PWM1 << 8), 0U,
		      "and channel 3's byte is untouched");
	zassert_equal(reg_rd(GPT_CCER) & CCER_CCxE(2U), CCER_CCxE(2U), "channel 2 enabled");
	zassert_equal(reg_rd(GPT_CCER) & CCER_CCxP(2U), CCER_CCxP(2U), "inverted polarity");

	zassert_ok(pwm_set_cycles(PWM_DEV, 3U, 4000U, 1000U, PWM_POLARITY_NORMAL));
	zassert_equal(reg_rd(GPT_CCMR1) & (OCM_PWM1 << 8), OCM_PWM1 << 8,
		      "channel 3 is the high byte of CCMR1");
	zassert_equal(reg_rd(GPT_CCER) & CCER_CCxP(3U), 0U, "channel 3 back to normal polarity");
	zassert_equal(reg_rd(GPT_CCER) & CCER_CCxP(2U), CCER_CCxP(2U),
		      "and channel 2 keeps its own");
}

/* A pulse longer than the period is refused with -EINVAL by both entry points:
 * z_impl_pwm_set_cycles() checks pulse > period itself
 * (include/zephyr/drivers/pwm.h), and the driver checks again so a caller that
 * goes straight through the api table gets the same answer instead of a
 * silently clamped 100% duty (the driver used to clamp).
 * Both halves are pinned here, including "the refusal happens before any
 * register is touched". */
ZTEST(pwm_agm_gptimer, test_22_pulse_longer_than_the_period)
{
	const struct pwm_driver_api *api = (const struct pwm_driver_api *)PWM_DEV->api;

	reg_wr(GPT_ARR, 0x1234U);  /* sentinel: a refused call must not touch it */
	reg_wr(GPT_CCR0 + 4U, 0x4321U);
	zassert_equal(pwm_set_cycles(PWM_DEV, 1U, 1000U, 5000U, PWM_POLARITY_NORMAL), -EINVAL,
		      "the public API refuses pulse > period");
	zassert_equal(reg_rd(GPT_ARR), 0x1234U, "and refuses it before touching ARR");
	zassert_equal(reg_rd(GPT_CCR0 + 4U), 0x4321U, "and before touching CCRx");

	zassert_equal(api->set_cycles(PWM_DEV, 1U, 1000U, 5000U, PWM_POLARITY_NORMAL), -EINVAL,
		      "the api table answers the same way: no clamp, no silent 100%");
	zassert_equal(reg_rd(GPT_ARR), 0x1234U, "and the driver's refusal touches nothing either");
	zassert_equal(reg_rd(GPT_CCR0 + 4U), 0x4321U, "CCRx included");

	/* The boundary itself stays legal -- pulse == period is exactly 100 %:
	 * CCRx = period while ARR = period - 1, so CNT < CCRx always holds. */
	zassert_ok(pwm_set_cycles(PWM_DEV, 1U, 1000U, 1000U, PWM_POLARITY_NORMAL),
		   "pulse == period is the 100 % case");
	zassert_equal(reg_rd(GPT_ARR), 999U, "period programmed");
	zassert_equal(reg_rd(GPT_CCR0 + 4U), 1000U, "and CCRx = period is what 100 % means here");
}

ZTEST(pwm_agm_gptimer, test_23_zero_period_disables_that_channel_only)
{
	zassert_ok(pwm_set_cycles(PWM_DEV, 0U, 1000U, 250U, PWM_POLARITY_NORMAL));
	zassert_ok(pwm_set_cycles(PWM_DEV, 1U, 1000U, 250U, PWM_POLARITY_NORMAL));

	zassert_ok(pwm_set_cycles(PWM_DEV, 0U, 0U, 0U, PWM_POLARITY_NORMAL));
	zassert_equal(reg_rd(GPT_CCER) & CCER_CCxE(0U), 0U, "channel 0 output disabled");
	zassert_equal(reg_rd(GPT_CCER) & CCER_CCxE(1U), CCER_CCxE(1U),
		      "channel 1 keeps running");
	zassert_equal(reg_rd(GPT_CR1) & CR1_CEN, CR1_CEN, "the counter keeps running");
}
