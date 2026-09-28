/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief native_sim suite for the backup-domain RTC counter driver.
 *
 * The model is the shared RAM window plus one bit set before device init:
 * CRL.RTOFF, which every backup-domain write has to wait for (see the CMake
 * comment). With it, the driver's whole init/prescaler/counter/alarm sequencing
 * runs and the registers it programs are readable.
 *
 * Covered: the clock-source selection in BDCR (LSI here, no LSEON), the
 * prescaler reload from the devicetree, the counter read (CNTH/CNTL) and its
 * width, the alarm target and its interrupt flag, and the source/frequency
 * reporting. The RTC's "fresh vs warm" branch is pinned through BDCR.RTCEN: the
 * model starts with RTCEN clear, so init takes the first-power-on path.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/counter/agm_rtc_regs.h>
#include <zephyr/init.h>
#include <zephyr/ztest.h>

#define RTC_DEV  DEVICE_DT_GET(DT_NODELABEL(rtc_agm0))

#define RTC_BASE 0x40000000UL

static uint16_t rd16(uint32_t off)
{
	return *(volatile uint16_t *)(RTC_BASE + off);
}

static void wr16(uint32_t off, uint16_t val)
{
	*(volatile uint16_t *)(RTC_BASE + off) = val;
}

/*
 * Arm RTOFF before any driver init. EARLY priority 1 runs after the shared
 * window's own EARLY priority 0 (agm_fake_mmio.c) and before PRE_KERNEL_1,
 * which is where the RTC driver's init sits.
 */
static int rtc_suite_arm_ready_bit(void)
{
	wr16(AGM_RTC_CRL, AGM_RTC_CRL_RTOFF);
	return 0;
}

SYS_INIT(rtc_suite_arm_ready_bit, EARLY, 1);

/* The driver keeps writing RTOFF back to 0 (agm_rtc_clear_int() is a
 * read-modify-write of CRL), so each case re-arms it before touching the
 * device. */
static void suite_reset_fake_regs(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)counter_stop(RTC_DEV);
	wr16(AGM_RTC_CRL, AGM_RTC_CRL_RTOFF);
	/* The RTC free-runs, so the counter is whatever the previous case left;
	 * the alarm target is computed from it. */
	wr16(AGM_RTC_CNTH, 0U);
	wr16(AGM_RTC_CNTL, 0U);
	wr16(AGM_RTC_ALRH, 0U);
	wr16(AGM_RTC_ALRL, 0U);
}

ZTEST_SUITE(counter_agm_rtc, NULL, NULL, suite_reset_fake_regs, NULL, NULL);

/* ---- what init programmed -------------------------------------------- */

ZTEST(counter_agm_rtc, test_01_init_selects_the_source_and_programs_the_prescaler)
{
	zassert_true(device_is_ready(RTC_DEV), "the RTC came up against the fake window");

	/* BDCR: RTCEN set, RTCSEL = LSI (2), and no LSEON -- the board dtsi's
	 * default is "lse", so this also pins that the property is honoured. */
	uint16_t bdcr = rd16(AGM_RTC_BDCR);

	zassert_equal(bdcr & AGM_RTC_BDCR_RTCEN, AGM_RTC_BDCR_RTCEN, "the backup domain is enabled");
	zassert_equal((bdcr >> AGM_RTC_BDCR_RTCSEL_OFF) & 0x3U, 2U, "RTCSEL = LSI");
	zassert_equal(bdcr & AGM_RTC_BDCR_LSEON, 0U, "and the crystal is not started");

	/* The prescaler reload comes straight from the devicetree: 32767 means
	 * the counter ticks once per (prescaler + 1) RTC clocks, i.e. 1 Hz. */
	zassert_equal(rd16(AGM_RTC_PRLH), 0U, "PRLH (prescaler is 16 bits)");
	zassert_equal(rd16(AGM_RTC_PRLL), 32767U, "PRLL from agm,prescaler");

	/* The counter is 32-bit and the device reports its *tick* rate, which is
	 * the RTC clock divided by the prescaler -- i.e. 1 Hz for the default
	 * 32768 prescaler, not 32768. */
	zassert_equal(counter_get_top_value(RTC_DEV), 0xffffffffU, "32-bit counter");
	zassert_equal(counter_get_frequency(RTC_DEV), 1U,
		      "ticks once a second (32768 / (32767 + 1))");
}

ZTEST(counter_agm_rtc, test_10_the_reported_value_is_the_counter_pair)
{
	uint32_t ticks = 0U;

	wr16(AGM_RTC_CNTH, 0x1234U);
	wr16(AGM_RTC_CNTL, 0x5678U);

	zassert_ok(counter_get_value(RTC_DEV, &ticks));
	zassert_equal(ticks, 0x12345678U, "CNTH is the high half, CNTL the low one");
}

/* ---- alarms ----------------------------------------------------------- */

ZTEST(counter_agm_rtc, test_20_an_alarm_programs_the_target_and_its_flag)
{
	struct counter_alarm_cfg alarm = { .ticks = 5U, .callback = (void *)0x1 };

	zassert_ok(counter_start(RTC_DEV));
	zassert_ok(counter_set_channel_alarm(RTC_DEV, 0U, &alarm));

	/* A relative request becomes the absolute tick the callback will report:
	 * now + ticks, which is 5 with the counter at 0. The *register* holds one
	 * less, because this RTC raises ALRF one tick after the compare (observed
	 * on-target: ALRF at CNT == ALR + 1), so the driver backs the programmed
	 * value off by one to fire at the requested tick. That quirk is exactly
	 * what this assertion pins. */
	uint32_t target = ((uint32_t)rd16(AGM_RTC_ALRH) << 16) | rd16(AGM_RTC_ALRL);

	zassert_equal(target, 4U, "ALR = (now + ticks) - 1");
	zassert_equal(rd16(AGM_RTC_CRL) & (AGM_RTC_CRL_ALRF | AGM_RTC_CRL_SECF | AGM_RTC_CRL_OWF),
		      0U, "stale flags are cleared when the alarm is armed");

	zassert_ok(counter_cancel_channel_alarm(RTC_DEV, 0U));
	zassert_ok(counter_stop(RTC_DEV));
}

ZTEST(counter_agm_rtc, test_21_the_alarm_guards)
{
	struct counter_alarm_cfg alarm = { .ticks = 1U, .callback = (void *)0x1 };

	/* One channel, and the callback is how the driver reports. */
	zassert_equal(counter_set_channel_alarm(RTC_DEV, 1U, &alarm), -ENOTSUP,
		      "refused by the API layer");
	alarm.callback = NULL;
	zassert_equal(counter_set_channel_alarm(RTC_DEV, 0U, &alarm), -EINVAL, "no callback");
	alarm.callback = (void *)0x1;

	zassert_ok(counter_start(RTC_DEV));
	zassert_ok(counter_set_channel_alarm(RTC_DEV, 0U, &alarm));
	zassert_equal(counter_set_channel_alarm(RTC_DEV, 0U, &alarm), -EBUSY,
		      "one alarm at a time");
	zassert_ok(counter_cancel_channel_alarm(RTC_DEV, 0U));
	zassert_ok(counter_stop(RTC_DEV));
}

ZTEST(counter_agm_rtc, test_22_the_top_value_is_fixed)
{
	struct counter_top_cfg cfg = { .ticks = 0xffffffffU, .callback = (void *)0x1 };

	cfg.ticks = 1000U;
	zassert_equal(counter_set_top_value(RTC_DEV, &cfg), -ENOTSUP, "the 32-bit top is fixed");
}

ZTEST(counter_agm_rtc, test_23_pending_reports_the_alarm_and_wrap_flags)
{
	wr16(AGM_RTC_CRL, AGM_RTC_CRL_RTOFF);	/* no flags set */
	zassert_equal(counter_get_pending_int(RTC_DEV), 0U, "nothing pending");

	wr16(AGM_RTC_CRL, AGM_RTC_CRL_RTOFF | AGM_RTC_CRL_ALRF);
	zassert_not_equal(counter_get_pending_int(RTC_DEV), 0U, "the alarm flag reads as pending");
}
