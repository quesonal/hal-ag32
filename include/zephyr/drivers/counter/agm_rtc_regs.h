/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief AgRV2K RTC / backup-domain register map (STM32F1-style).
 *
 * One copy, shared by everything that touches the RTC block:
 *
 *   - drivers/counter/counter_agm_rtc.c, which owns the peripheral when it is
 *     enabled (the RTC node is `okay`),
 *   - drivers/watchdog/wdt_iwdg_agm.c, whose IWDG is a second register in the
 *     same backup domain, and
 *   - drivers/misc/boot_agm.c, whose one-shot boot override writes two backup
 *     registers and whose trial-boot handshake writes two more. It does that
 *     directly, on purpose: the loader must not pull the counter driver -- and
 *     with it an LSE start-up wait of up to a second on every boot -- into an
 *     image that needs neither the counter nor the LSE. Applications do the
 *     same thing through the inline helpers in
 *     <zephyr/drivers/misc/boot_agm.h>, which is why this header is public:
 *     one copy of the map, or the bootloader and its images drift apart --
 *     which is exactly what happened once: the loader had CRL at 0x08 and the
 *     backup registers at 0x20.
 *
 *     CONFIG_BOOT_AGM_ONESHOT is `depends on !COUNTER_AGM_RTC`, so the same
 *     image never has two writers of the RTC clock configuration.
 *
 * Offsets are relative to the RTC node's base (0x40000000 on this SoC); every
 * register is 16-bit at a 4-byte stride. Layout taken from the vendor SDK
 * (framework-agrv_sdk/src/rtc.h: `CRL` at 0x04, `BKP_DR[16]` at 0x40) -- the
 * bootloader driver used to keep its own copy with CRL at 0x08 and the backup
 * registers at 0x20, i.e. it waited on a prescaler bit and stored its magic in
 * the alarm registers.
 */

#ifndef DRIVERS_COUNTER_AGM_RTC_REGS_H_
#define DRIVERS_COUNTER_AGM_RTC_REGS_H_

#include <zephyr/sys/util.h>

/* Register offsets (16-bit registers at 4-byte stride). */
#define AGM_RTC_CRH		0x00U
#define AGM_RTC_CRL		0x04U
#define AGM_RTC_PRLH		0x08U
#define AGM_RTC_PRLL		0x0cU
#define AGM_RTC_DIVH		0x10U
#define AGM_RTC_DIVL		0x14U
#define AGM_RTC_CNTH		0x18U
#define AGM_RTC_CNTL		0x1cU
#define AGM_RTC_ALRH		0x20U
#define AGM_RTC_ALRL		0x24U
#define AGM_RTC_BDCR		0x30U
#define AGM_RTC_BDRST		0x32U
/* BDRST bit 0: pulse it to clear the whole backup domain (vendor SDK
 * rtc.c::RTC_Reset(), which the IWDG driver needs before its register
 * write latches). */
#define AGM_RTC_BDRST_BIT	BIT(0)

/* CRH bits. */
#define AGM_RTC_CRH_SECIE	BIT(0)
#define AGM_RTC_CRH_ALRIE	BIT(1)
#define AGM_RTC_CRH_OWIE	BIT(2)

/* CRL bits. */
#define AGM_RTC_CRL_SECF	BIT(0)
#define AGM_RTC_CRL_ALRF	BIT(1)
#define AGM_RTC_CRL_OWF		BIT(2)
#define AGM_RTC_CRL_RTOFF	BIT(5)

/* BDCR bits (SDK rtc.h RTC_BDCR_*). */
#define AGM_RTC_BDCR_LSEON	BIT(0)
#define AGM_RTC_BDCR_LSERDY	BIT(1)
#define AGM_RTC_BDCR_RTCEN	BIT(15)
#define AGM_RTC_BDCR_RTCSEL_OFF	8
#define AGM_RTC_BDCR_RTCSEL_MSK	(3U << AGM_RTC_BDCR_RTCSEL_OFF)

#define AGM_RTC_SRC_LSE		(1U << AGM_RTC_BDCR_RTCSEL_OFF)
#define AGM_RTC_SRC_LSI		(2U << AGM_RTC_BDCR_RTCSEL_OFF)
#define AGM_RTC_SRC_LOCAL	(3U << AGM_RTC_BDCR_RTCSEL_OFF)

/* Backup data registers: 16 halfwords, so the block runs 0x40..0x80. Every
 * write to them (or to any other backup-domain register) has to be followed by
 * a wait for CRL.RTOFF -- the vendor's RTC_WRITE_REG() does the same. */
#define AGM_RTC_BKP_DR_COUNT	16U
#define AGM_RTC_BKP_DR(idx)	(0x40U + (idx) * 4U)

/* IWDG (independent watchdog) is a 16-bit register of the same backup domain,
 * at RTC base + 0x34 (vendor SDK framework-agrv_sdk/src/rtc.h
 * `RTC_TypeDef.IWDG`, iwdg.h for the bits). Write-only in the sense that
 * matters: there is no readable reload value, and a feed writes the key
 * 0xa000 into bits 15:12. The bit names are the SDK's. */
#define AGM_RTC_IWDG		0x34U
#define AGM_RTC_IWDG_PRESCALER_MASK	0x7U
#define AGM_RTC_IWDG_STOP_FREEZE	BIT(4)
#define AGM_RTC_IWDG_STANDBY_FREEZE	BIT(5)
#define AGM_RTC_IWDG_CLKSEL		BIT(6)
#define AGM_RTC_IWDG_ENABLE		BIT(8)
#define AGM_RTC_IWDG_RELOAD_MASK	0xf000U
#define AGM_RTC_IWDG_RELOAD_KEY		0xa000U

#endif /* DRIVERS_COUNTER_AGM_RTC_REGS_H_ */
