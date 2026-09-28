/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (c) 2026 AgRV Contributors
 *
 * AgRV2K SYS controller + GPIO block addresses, shared by the SoC glue
 * (soc.c) and the peripheral drivers (drivers/{i2c,pwm,counter,can,
 * watchdog,usb}) so there is exactly ONE C-side definition of each
 * address. Before this header every driver carried its own copy of
 * 0x03000000 / 0x40014000 / the GPIO AFSEL offset, which meant the
 * devicetree and the drivers could silently disagree .
 *
 * Rule of thumb: only *silicon-wide* constants live here. Anything a
 * board can change — instance index, pin routing, APB/AHB gate bit for
 * a specific instance — belongs in devicetree, not in this header.
 * Sources: AgRV SDK framework-agrv_sdk/src/AltaRiscv.h (SYS_TypeDef,
 * GPIO_BASE, APB_MASK_GPIO0..9); the GPIO bank count (10 banks of 8)
 * is corroborated by the SDK layout — GPIO8_BASE / GPIO9_BASE at
 * 0x4001C000 / 0x4001D000 and the APB_MASK_GPIO8 / APB_MASK_GPIO9
 * entries in the same header.
 */

#ifndef ZEPHYR_SOC_AGM_AGRV2K_AGM_SYS_H_
#define ZEPHYR_SOC_AGM_AGRV2K_AGM_SYS_H_

/* --- SYS controller @ 0x03000000 ---------------------------------------- */
#define AGM_SYS_BASE		0x03000000UL
#define AGM_SYS_AHB_RESET	0x50U	/* write 1 = assert reset */
#define AGM_SYS_RST_CNTL	0x04U	/* bit 0 = software reset (SDK SYS_RST_SFT) */
#define AGM_SYS_RST_SFT		BIT(0)	/* SYS_SoftwareReset() in the SDK */
/* Reset-cause flags in RST_CNTL, cleared by writing AGM_SYS_RST_REMOVE back
 * with them (SDK system.h SYS_RSTF_* / SYS_RST_REMOVE). The two watchdogs have
 * a bit each: WDOG0 is the SP805-style system one, IWDG the backup-domain one
 * a trial boot arms. */
#define AGM_SYS_RSTF_IWDG	BIT(29)	/* SYS_RSTF_IWDG */
#define AGM_SYS_RSTF_WDOG	BIT(30)	/* SYS_RSTF_WDOG */
#define AGM_SYS_RST_REMOVE	BIT(24)	/* write 1 to clear the flags */
#define AGM_SYS_APB_CLKENABLE	0x60U	/* APB peripheral clock enable */
#define AGM_SYS_AHB_CLKENABLE	0x70U	/* AHB peripheral clock enable */
#define AGM_SYS_APB_CLKSTOP	0x80U	/* APB clock stop while core halted */

/* SYS.CLK_CNTL: clock source select (the low two bits) plus the HSE/PLL
 * enable and ready flags the switch sequences poll. The core's own cycle
 * counter (`mcycle`) counts whatever this selects, so arch_busy_wait()
 * (busy_wait.c) reads the same register to know what a waiting microsecond
 * costs -- the vendor SDK's SYS_GetSysClkFreq() does exactly this
 * (framework-agrv_sdk/src/system.c). */
#define AGM_SYS_CLK_CNTL	0x0CU
/* Same register, spelled with the base: bit 12..15 of it is SCLK_DIV_HIGH, the
 * divider the *flash controller* uses for its serial clock (SDK FLASH_Start()
 * waits SCLK_DIV_HIGH * 256 cycles before asserting STRT). Kept here so
 * flash_agm.c does not have to open-code the SYS base and offset. */
#define AGM_SYS_CLK_CNTL_ADDR	(AGM_SYS_BASE + AGM_SYS_CLK_CNTL)
#define AGM_SYS_CLK_SOURCE_MASK	0x3U
#define AGM_SYS_CLK_SOURCE_HSI	0x0U
#define AGM_SYS_CLK_SOURCE_HSE	0x1U
#define AGM_SYS_CLK_SOURCE_PLL	0x2U
#define AGM_SYS_CLK_HSE_ON	BIT(2)
#define AGM_SYS_CLK_HSE_BYP	BIT(3)
#define AGM_SYS_CLK_HSE_RDY	BIT(4)
#define AGM_SYS_CLK_PLL_ON	BIT(5)
#define AGM_SYS_CLK_PLL_RDY	BIT(6)

/* SYS.APB_CLKENABLE bits — common peripherals. */
#define AGM_APB_CLK_FCB0	(1UL << 0)
#define AGM_APB_CLK_WDOG0	(1UL << 1)
#define AGM_APB_CLK_UART0	(1UL << 21)
#define AGM_APB_CLK_CAN0	(1UL << 26)
#define AGM_APB_CLK_I2C0	(1UL << 27)
#define AGM_APB_CLK_I2C1	(1UL << 28)

/* Every APB peripheral page is 4 KiB: base(n) = base(0) + n * stride. */
#define AGM_APB_STRIDE		0x1000UL

/* --- GPIO: 10 banks x 8 bit, one 4 KiB page each ------------------------ *
 * AgRV2K GPIO block (10 banks of 8 pins; per the SDK's GPIO_BASE /
 * GPIO8_BASE / GPIO9_BASE layout). Registers are Stellaris-style:
 * DATA[256] masked-write @base, DIR +0x400, IS/IBE/IEV/IE/RIS/MIS/IC
 * +0x404..0x41C, AFSEL +0x420.
 */
#define AGM_GPIO0_BASE		0x40014000UL
#define AGM_GPIO_BANK_COUNT	10U
#define AGM_GPIO_PINS_PER_BANK	8U
#define AGM_GPIO_BANK(n)	(AGM_GPIO0_BASE + ((n) * AGM_APB_STRIDE))
#define AGM_GPIO_DIR_OFF	0x400U
#define AGM_GPIO_AFSEL_OFF	0x420U
#define AGM_GPIO_AFSEL(n)	(AGM_GPIO_BANK(n) + AGM_GPIO_AFSEL_OFF)

/* SYS.APB_CLKENABLE bit gating GPIO bank n (AltaRiscv.h APB_MASK_GPIO0..9
 * are contiguous: GPIO0 = bit 4 ... GPIO9 = bit 13). */
#define AGM_APB_CLK_GPIO(n)	(1UL << (4U + (n)))

/* --- APB peripherals ---------------------------------------------------- */
/* I2C0/1 @ 0x4002B000/0x4002C000; APB gate = bit (27 + idx). */
#define AGM_I2C_BASE(n)		(0x4002b000UL + ((n) * AGM_APB_STRIDE))

/* GPTIMER0..4 @ 0x40020000..0x40024000; APB gate = bit (16 + idx). */
#define AGM_GPTIMER_BASE(n)	(0x40020000UL + ((n) * AGM_APB_STRIDE))
#define AGM_APB_CLK_GPTIMER(n)	(1UL << (16U + (n)))

/* Dual sub-timers TIMER0/1 @ 0x4001E000/0x4001F000; APB gate = bit (14 + idx). */
#define AGM_TIMER_BASE(n)	(0x4001E000UL + ((n) * AGM_APB_STRIDE))
#define AGM_APB_CLK_TIMER(n)	(1UL << (14U + (n)))

#endif /* ZEPHYR_SOC_AGM_AGRV2K_AGM_SYS_H_ */
