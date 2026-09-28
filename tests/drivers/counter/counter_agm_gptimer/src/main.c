/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief native_sim suite for the GPTIMER counter driver.
 *
 * The model is the passive one (RAM window, tests/drivers/common/agm_native)
 * plus the two registers this driver reads: CNT, which is what it reports and
 * what alarms are computed against, and SR, the compare/wrap flags it clears.
 * Setting CNT from the test is what makes the alarm arithmetic checkable: the
 * driver converts a relative or absolute request into a compare value, and the
 * register it programs (CCR0) is exactly the answer.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/ztest.h>

#define CNT_DEV  DEVICE_DT_GET(DT_NODELABEL(gptimer_counter0))

/* Register offsets (drivers/counter/counter_agm_gptimer.c). */
#define R_CR1   0x00U
#define R_DIER  0x0cU
#define R_SR    0x10U
#define R_CCMR0 0x18U
#define R_CCER  0x20U
#define R_CNT   0x24U
#define R_PSC   0x28U
#define R_ARR   0x2cU
#define R_CCR0  0x34U

#define CR1_CEN    BIT(0)
#define DIER_UIE   BIT(0)
#define DIER_CC0IE BIT(1)
#define SR_UIF     BIT(0)
#define SR_CC0IF   BIT(1)
#define EGR_UG     BIT(0)
#define CCER_CC0E  BIT(0)

#define GPT_BASE 0x40020000UL

static uint32_t rd(uint32_t off)
{
	return *(volatile uint32_t *)(GPT_BASE + off);
}

static void wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(GPT_BASE + off) = val;
}

static void suite_reset_fake_regs(void *fixture)
{
	ARG_UNUSED(fixture);

	/* Disarm first: the callback/dier state lives in device data, so a case
	 * that leaves an alarm armed would make the next one answer -EBUSY. */
	(void)counter_stop(CNT_DEV);

	wr(R_CNT, 0U);
	wr(R_SR, 0U);
	wr(R_DIER, 0U);
	wr(R_CCR0, 0U);
}

ZTEST_SUITE(counter_agm_gptimer, NULL, NULL, suite_reset_fake_regs, NULL, NULL);

/* ---- init and the free-running configuration -------------------------- */

ZTEST(counter_agm_gptimer, test_01_init_sets_up_the_free_running_counter)
{
	zassert_true(device_is_ready(CNT_DEV), "the counter came up against the fake window");

	/* Free-run 0..0xffffffff, prescaler /1, compare channel 0 in toggle mode
	 * (its OC output is never routed to a pin, so it only makes CC0IF). */
	zassert_equal(rd(R_PSC), 0U, "prescaler /1");
	zassert_equal(rd(R_ARR), 0xffffffffU, "the 32-bit top is the auto-reload value");
	/* OC0M = 0b011 (toggle) -- not the 0b111 mask value: the channel only
	 * has to make CC0IF, its output never reaches a pin. */
	zassert_equal(rd(R_CCMR0), (3U << 4), "OC0M = toggle, the compare event an alarm uses");
	zassert_equal(rd(R_CCER) & CCER_CC0E, CCER_CC0E, "compare channel 0 enabled");
	zassert_equal(rd(R_DIER), 0U, "no interrupts until something asks for one");

	zassert_equal(counter_get_top_value(CNT_DEV), 0xffffffffU, "the top is fixed");
	zassert_equal(counter_get_frequency(CNT_DEV), 200000000U, "the counter ticks at pclk");
}

ZTEST(counter_agm_gptimer, test_10_start_and_stop_toggle_the_counter_enable)
{
	zassert_ok(counter_start(CNT_DEV));
	zassert_equal(rd(R_CR1) & CR1_CEN, CR1_CEN, "start enables the counter");

	zassert_ok(counter_stop(CNT_DEV));
	zassert_equal(rd(R_CR1) & CR1_CEN, 0U, "stop disables it");
	zassert_equal(rd(R_DIER) & DIER_CC0IE, 0U, "and drops the compare interrupt");
}

/* ---- alarms: the arithmetic is the observable ------------------------- */

ZTEST(counter_agm_gptimer, test_20_a_relative_alarm_arms_the_compare_value)
{
	struct counter_alarm_cfg alarm = { .ticks = 1000U, .flags = 0U, .callback = (void *)0x1,
					   .user_data = NULL };

	wr(R_CNT, 500U);
	zassert_ok(counter_start(CNT_DEV));
	wr(R_SR, SR_CC0IF);	/* a stale flag the driver has to clear */

	zassert_ok(counter_set_channel_alarm(CNT_DEV, 0U, &alarm));

	zassert_equal(rd(R_CCR0), 1500U, "now + ticks");
	zassert_equal(rd(R_DIER) & DIER_CC0IE, DIER_CC0IE, "the compare interrupt is armed");
	zassert_equal(rd(R_SR) & SR_CC0IF, 0U, "and the stale flag was cleared");
}

ZTEST(counter_agm_gptimer, test_21_an_absolute_alarm_uses_the_tick_as_given)
{
	struct counter_alarm_cfg alarm = { .ticks = 7000U, .callback = (void *)0x1,
					   .flags = COUNTER_ALARM_CFG_ABSOLUTE };

	wr(R_CNT, 500U);
	zassert_ok(counter_start(CNT_DEV));
	zassert_ok(counter_set_channel_alarm(CNT_DEV, 0U, &alarm));

	zassert_equal(rd(R_CCR0), 7000U, "an absolute alarm is the value itself");
	zassert_ok(counter_cancel_channel_alarm(CNT_DEV, 0U));
}

ZTEST(counter_agm_gptimer, test_22_an_alarm_due_now_expires_on_the_next_tick)
{
	struct counter_alarm_cfg alarm = { .ticks = 500U, .callback = (void *)0x1,
					   .flags = COUNTER_ALARM_CFG_ABSOLUTE };

	wr(R_CNT, 500U);	/* the alarm is due this very tick */
	zassert_ok(counter_start(CNT_DEV));
	zassert_ok(counter_set_channel_alarm(CNT_DEV, 0U, &alarm));

	zassert_equal(rd(R_CCR0), 501U, "a zero-distance alarm is pushed one tick out");
	zassert_ok(counter_cancel_channel_alarm(CNT_DEV, 0U));
}

ZTEST(counter_agm_gptimer, test_23_the_alarm_guards_and_the_cancel)
{
	struct counter_alarm_cfg alarm = { .ticks = 10U, .callback = (void *)0x1 };

	/* Only channel 0 exists. The *public* API answers -ENOTSUP for an
	 * out-of-range channel before the driver sees it, so the driver's own
	 * -EINVAL check is unreachable through counter_set_channel_alarm()
	 * (include/zephyr/drivers/counter.h: z_impl_counter_set_channel_alarm). */
	zassert_equal(counter_set_channel_alarm(CNT_DEV, 1U, &alarm), -ENOTSUP,
		      "one channel, refused by the API layer");
	alarm.callback = NULL;
	zassert_equal(counter_set_channel_alarm(CNT_DEV, 0U, &alarm), -EINVAL, "no callback");
	alarm.callback = (void *)0x1;

	/* A stopped counter cannot host an alarm. */
	zassert_ok(counter_stop(CNT_DEV));
	zassert_equal(counter_set_channel_alarm(CNT_DEV, 0U, &alarm), -EIO, "not started");

	zassert_ok(counter_start(CNT_DEV));
	zassert_ok(counter_set_channel_alarm(CNT_DEV, 0U, &alarm));
	zassert_equal(counter_set_channel_alarm(CNT_DEV, 0U, &alarm), -EBUSY,
		      "one alarm at a time");

	zassert_ok(counter_cancel_channel_alarm(CNT_DEV, 0U));
	zassert_equal(rd(R_DIER) & DIER_CC0IE, 0U, "cancelling drops the compare interrupt");
	zassert_equal(rd(R_SR) & SR_CC0IF, 0U, "and clears the flag");

	/* After a cancel the channel is free again. */
	zassert_ok(counter_set_channel_alarm(CNT_DEV, 0U, &alarm));
	zassert_ok(counter_cancel_channel_alarm(CNT_DEV, 0U));
}

ZTEST(counter_agm_gptimer, test_24_the_value_is_the_counter_register)
{
	uint32_t ticks = 0U;

	wr(R_CNT, 0x12345678U);
	zassert_ok(counter_get_value(CNT_DEV, &ticks));
	zassert_equal(ticks, 0x12345678U, "get_value reads CNT");
}

ZTEST(counter_agm_gptimer, test_26_the_whole_32bit_range_is_a_valid_alarm)
{
	struct counter_alarm_cfg alarm = {
		.ticks = UINT32_MAX,
		.flags = COUNTER_ALARM_CFG_ABSOLUTE,
		.callback = (void *)0x1,
	};

	/* The driver used to test `alarm_cfg->ticks > AGM_GPT_TOP` with
	 * AGM_GPT_TOP == UINT32_MAX, i.e. a condition that can never be true.
	 * Pin the real contract instead: the full 32-bit range
	 * is representable, so the top of it is accepted and programmed into the
	 * compare register. */
	zassert_ok(counter_start(CNT_DEV), "the counter must run to host an alarm");
	zassert_ok(counter_set_channel_alarm(CNT_DEV, 0U, &alarm),
		   "an absolute target at the top of the range is legal");
	zassert_equal(rd(R_CCR0), UINT32_MAX, "and it lands in CCR0");
	zassert_ok(counter_cancel_channel_alarm(CNT_DEV, 0U));
}

/* ---- the top callback and the pending flags --------------------------- */

ZTEST(counter_agm_gptimer, test_30_top_value_is_fixed_and_gates_the_wrap_interrupt)
{
	struct counter_top_cfg cfg = { .ticks = 0xffffffffU, .callback = (void *)0x1 };

	/* Only the 32-bit free-run top is supported. */
	cfg.ticks = 0x1000U;
	zassert_equal(counter_set_top_value(CNT_DEV, &cfg), -ENOTSUP, "the top is fixed");
	cfg.ticks = 0xffffffffU;

	wr(R_SR, SR_UIF);	/* stale wrap flag */
	zassert_ok(counter_set_top_value(CNT_DEV, &cfg));
	zassert_equal(rd(R_DIER) & DIER_UIE, DIER_UIE, "the wrap interrupt is enabled");
	zassert_equal(rd(R_SR) & SR_UIF, 0U, "and the stale flag cleared");

	/* Removing the callback disables it again. */
	cfg.callback = NULL;
	zassert_ok(counter_set_top_value(CNT_DEV, &cfg));
	zassert_equal(rd(R_DIER) & DIER_UIE, 0U, "no callback, no wrap interrupt");

	/* An armed alarm blocks a top-value change (they share the compare). */
	cfg.callback = (void *)0x1;
	zassert_ok(counter_set_top_value(CNT_DEV, &cfg));
	struct counter_alarm_cfg alarm = { .ticks = 10U, .callback = (void *)0x1 };

	zassert_ok(counter_start(CNT_DEV));
	zassert_ok(counter_set_channel_alarm(CNT_DEV, 0U, &alarm));
	zassert_equal(counter_set_top_value(CNT_DEV, &cfg), -EBUSY, "an alarm is armed");
	zassert_ok(counter_cancel_channel_alarm(CNT_DEV, 0U));
}

ZTEST(counter_agm_gptimer, test_31_pending_reports_the_compare_and_wrap_flags)
{
	wr(R_SR, 0U);
	zassert_equal(counter_get_pending_int(CNT_DEV), 0U, "nothing pending");

	wr(R_SR, SR_CC0IF);
	zassert_equal(counter_get_pending_int(CNT_DEV), SR_CC0IF, "the compare flag");

	wr(R_SR, SR_UIF);
	zassert_equal(counter_get_pending_int(CNT_DEV), SR_UIF, "the wrap flag");
}
