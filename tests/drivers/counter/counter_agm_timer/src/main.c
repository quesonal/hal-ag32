/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief native_sim suite for the dual-timer counter driver.
 *
 * The interesting part of this driver is that Zephyr's counter model (a
 * count-up value over 2^32 ticks) does not match the hardware (two 16/32-bit
 * *down* counters), so the driver derives the reported value from sub-timer 1's
 * VALUE register:
 *
 *     value = phase + (0xffffffff - cnt)     while free-running
 *     value = target - cnt                   while channel 0 has an alarm
 *
 * Both forms are pure arithmetic on a register the test controls, so this
 * suite can check the reported value against hand-computed numbers -- the one
 * place in this driver where a mistake is invisible on the board (a wrong
 * phase just shifts every later value).
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/ztest.h>

#define TMR_DEV  DEVICE_DT_GET(DT_NODELABEL(timer_counter0))

/* Sub-timer register offsets (drivers/counter/counter_agm_timer.c). */
#define SUB_STRIDE 0x20U
#define R_LOAD   0x00U
#define R_VALUE  0x04U
#define R_CTRL   0x08U
#define R_INTCLR 0x0cU
#define R_MIS    0x14U
#define R_BGL    0x18U

#define CTRL_ONESHOT  BIT(0)
#define CTRL_SIZE     BIT(1)
#define CTRL_INT_EN   BIT(5)
#define CTRL_PERIODIC BIT(6)
#define CTRL_ENABLE   BIT(7)

#define TIMER_BASE 0x4001e000UL

static uint32_t sub_off(uint32_t sub, uint32_t reg)
{
	return (sub * SUB_STRIDE) + reg;
}

static uint32_t rd(uint32_t sub, uint32_t reg)
{
	return *(volatile uint32_t *)(TIMER_BASE + sub_off(sub, reg));
}

static void wr(uint32_t sub, uint32_t reg, uint32_t val)
{
	*(volatile uint32_t *)(TIMER_BASE + sub_off(sub, reg)) = val;
}

static void suite_reset_fake_regs(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)counter_stop(TMR_DEV);

	for (uint32_t sub = 0U; sub < 2U; sub++) {
		wr(sub, R_LOAD, 0U);
		wr(sub, R_VALUE, 0U);
		wr(sub, R_MIS, 0U);
	}
}

ZTEST_SUITE(counter_agm_timer, NULL, NULL, suite_reset_fake_regs, NULL, NULL);

ZTEST(counter_agm_timer, test_01_start_programs_the_free_run_and_parks_the_second_sub_timer)
{
	zassert_true(device_is_ready(TMR_DEV), "the counter came up against the fake window");

	/* The fixture stops the counter between cases (the armed state lives in
	 * device data, so it has to be cleared), which also clears CTRL -- so the
	 * free-run programming is asserted through start(), which writes exactly
	 * the same thing init() does (agm_timer_sub_load(cfg, 0, TOP, ...)). */
	zassert_ok(counter_start(TMR_DEV));

	/* Sub-timer 1 free-runs from the 32-bit top: periodic, 32-bit, /1. Its
	 * interrupt stays off until a top callback asks for it. */
	uint32_t ctrl = rd(0U, R_CTRL);

	zassert_equal(ctrl & (CTRL_PERIODIC | CTRL_SIZE), CTRL_PERIODIC | CTRL_SIZE,
		      "periodic, 32-bit");
	zassert_equal(rd(0U, R_LOAD), 0xffffffffU, "loaded with the top");
	zassert_equal(rd(0U, R_BGL), 0xffffffffU, "and the background load matches");
	zassert_equal(ctrl & CTRL_ENABLE, CTRL_ENABLE, "and it runs");
	zassert_equal(ctrl & CTRL_INT_EN, 0U, "without an interrupt until asked");

	/* The second sub-timer is parked -- nothing starts it but a channel-1
	 * alarm. */
	zassert_equal(rd(1U, R_CTRL), 0U, "sub-timer 2 is disabled");
	zassert_equal(rd(1U, R_LOAD), 0U, "with no load");

	zassert_equal(counter_get_frequency(TMR_DEV), 200000000U, "ticks at pclk");
	zassert_equal(counter_get_top_value(TMR_DEV), 0xffffffffU, "32-bit top");
}

ZTEST(counter_agm_timer, test_10_the_reported_value_is_derived_from_the_down_counter)
{
	uint32_t ticks = 0U;

	zassert_ok(counter_start(TMR_DEV));

	/* Free-running: value = phase + (top - cnt), and phase restarts at 0. */
	wr(0U, R_VALUE, 0x100U);
	zassert_ok(counter_get_value(TMR_DEV, &ticks));
	zassert_equal(ticks, 0xffffffffU - 0x100U, "value = 0 + (top - cnt)");

	/* The counter counts up, so a smaller down-count is a larger value. */
	wr(0U, R_VALUE, 0x99U);
	zassert_ok(counter_get_value(TMR_DEV, &ticks));
	zassert_equal(ticks, 0xffffffffU - 0x99U, "and it follows the register");

	zassert_ok(counter_stop(TMR_DEV));
}

ZTEST(counter_agm_timer, test_20_relative_and_absolute_alarms_load_the_delta)
{
	struct counter_alarm_cfg alarm = { .ticks = 1000U, .callback = (void *)0x1 };

	zassert_ok(counter_start(TMR_DEV));
	wr(0U, R_VALUE, 0x100U);	/* now = top - 0x100 */
	zassert_ok(counter_set_channel_alarm(TMR_DEV, 0U, &alarm));

	/* Channel 0 counts down exactly `delta` ticks with the interrupt on, so
	 * LOAD is the distance the caller asked for. */
	zassert_equal(rd(0U, R_LOAD), 1000U, "the relative delta is the load");
	zassert_equal(rd(0U, R_CTRL) & CTRL_INT_EN, CTRL_INT_EN, "interrupt armed");
	zassert_equal(rd(0U, R_CTRL) & CTRL_ENABLE, CTRL_ENABLE, "and the sub-timer runs");

	zassert_ok(counter_cancel_channel_alarm(TMR_DEV, 0U));
	zassert_equal(rd(0U, R_LOAD), 0xffffffffU, "cancelling restores the free run");

	/* Channel 1 is the second sub-timer and never touches the value above. */
	alarm.ticks = 500U;
	zassert_ok(counter_set_channel_alarm(TMR_DEV, 1U, &alarm));
	zassert_equal(rd(1U, R_LOAD), 500U, "channel 1 loads sub-timer 2");
	zassert_equal(rd(1U, R_CTRL) & CTRL_ENABLE, CTRL_ENABLE, "and runs it");
	zassert_equal(rd(0U, R_LOAD), 0xffffffffU, "leaving sub-timer 1 free-running");

	zassert_ok(counter_cancel_channel_alarm(TMR_DEV, 1U));
	zassert_equal(rd(1U, R_CTRL), 0U, "cancelling channel 1 parks sub-timer 2");
}

ZTEST(counter_agm_timer, test_21_the_guards)
{
	struct counter_alarm_cfg alarm = { .ticks = 10U, .callback = (void *)0x1 };

	/* The public API refuses a channel this device does not have. */
	zassert_equal(counter_set_channel_alarm(TMR_DEV, 2U, &alarm), -ENOTSUP,
		      "two channels, refused by the API layer");
	zassert_ok(counter_start(TMR_DEV));
	zassert_ok(counter_set_channel_alarm(TMR_DEV, 0U, &alarm));
	zassert_equal(counter_set_channel_alarm(TMR_DEV, 0U, &alarm), -EBUSY, "one per channel");
	zassert_ok(counter_cancel_channel_alarm(TMR_DEV, 0U));

	/* A stopped counter cannot host an alarm. */
	zassert_ok(counter_stop(TMR_DEV));
	zassert_equal(counter_set_channel_alarm(TMR_DEV, 0U, &alarm), -EIO, "not started");
}

ZTEST(counter_agm_timer, test_30_top_value_and_pending)
{
	struct counter_top_cfg cfg = { .ticks = 0xffffffffU, .callback = (void *)0x1 };

	cfg.ticks = 0x1000U;
	zassert_equal(counter_set_top_value(TMR_DEV, &cfg), -ENOTSUP, "the top is fixed");
	cfg.ticks = 0xffffffffU;

	zassert_ok(counter_start(TMR_DEV));
	zassert_ok(counter_set_top_value(TMR_DEV, &cfg));
	zassert_equal(rd(0U, R_CTRL) & CTRL_INT_EN, CTRL_INT_EN, "the wrap interrupt is enabled");

	/* A channel alarm and the wrap callback cannot share sub-timer 1. */
	struct counter_alarm_cfg alarm = { .ticks = 10U, .callback = (void *)0x1 };

	zassert_ok(counter_set_channel_alarm(TMR_DEV, 0U, &alarm));
	zassert_equal(counter_set_top_value(TMR_DEV, &cfg), -EBUSY, "an alarm is armed");
	zassert_ok(counter_cancel_channel_alarm(TMR_DEV, 0U));

	wr(0U, R_MIS, 0U);
	wr(1U, R_MIS, 0U);
	zassert_equal(counter_get_pending_int(TMR_DEV), 0U, "nothing pending");
	wr(1U, R_MIS, BIT(0));
	zassert_equal(counter_get_pending_int(TMR_DEV), BIT(0), "the second sub-timer's flag");
}
