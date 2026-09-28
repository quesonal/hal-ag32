/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AgRV2K SoC PM hooks — power management.
 *
 * Maps Zephyr PM states to the AG32 low-power modes exposed by the AgRV
 * SDK (framework-agrv_sdk/src/system.c::SYS_EnterSleepMode /
 * SYS_EnterStopMode / SYS_EnterStandbyMode, system.h PWR_CNTL field):
 *
 *   PM_STATE_SUSPEND_TO_IDLE -> SYS_SLEEP_MODE   (PWR_CNTL 0b00)
 *   PM_STATE_STANDBY         -> SYS_STOP_MODE    (PWR_CNTL 0b01)
 *   PM_STATE_SOFT_OFF        -> SYS_STANDBY_MODE (PWR_CNTL 0b11)
 *
 * Entry is always WFI with interrupts masked: this kernel PM path keeps
 * IRQs locked across pm_state_set() (see pm.h @ref pm_state_set), so we
 * only arm the low-power mode and wait. Wakeup:
 *
 *   - sleep: any enabled interrupt, including MTIP — the CLINT keeps
 *     running, so the tickless kernel resumes with no time drift.
 *   - stop: only the EXT_INTx wake lines (.ve-routed) or the RTC alarm.
 *     The shipped example_board.bin ties every EXT_INTx to GND
 *     (boards/agm/agrv2k_407/board.ve). The RTC alarm path is armed
 *     here: before WFI the driver-side ALRIE (an alarm armed through
 *     the counter API) is mirrored onto the SYS wake-up controller
 *     (WKP line 8, rising edge — SDK SYS_SetAlarmWakeupEdge) and stale
 *     pending wake-ups are cleared. Stop is force-only and disabled by
 *     default in the dtsi. If neither an RTC alarm nor any EXT_INTx
 *     wake edge is armed, stop would lock the SoC until power-on
 *     reset, so pm_state_set() falls back to sleep mode and warns.
 *   - standby: 1.2 V domain off. Wake behavior depends on the FLASH
 *     user option byte STDBY_NO_RST (system.h SYS_RSTF_LPWR vs SFT):
 *     with the bit cleared the wake is a power-on reset
 *     (SYS_RSTF_LPWR) and pm_state_set() never returns; with the bit
 *     set (factory default on agrv2k_407) the core resumes after the
 *     WFI below and we force a software reset (SYS_RSTF_SFT on the
 *     next boot) so the image always restarts from a known state.
 *
 * Exit from stop: the SDK switches to HSI before WFI so a deterministic
 * clock exists on wake; pm_state_exit_post_ops() restores the PLL with
 * the same sequence as the SDK's SYS_SwitchPLLClock after stop
 * (example_system.c::TestStop).
 */

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/state.h>
#include <zephyr/sys/util.h>

#include "agm_sys.h"

#if defined(CONFIG_PM)

LOG_MODULE_REGISTER(agrv2k_pm, LOG_LEVEL_INF);

/* SYS controller address + offsets are shared with the drivers
 * (agm_sys.h); same layout as soc.c / clk.c. */
#define SYS_BASE      AGM_SYS_BASE
#define SYS_RST_CNTL  0x04U  /* Reset control register */
#define SYS_PWR_CNTL  0x08U  /* Power control register */
#define SYS_DBG_CNTL  0x1CU  /* Debug control register */
#define SYS_WKP_RISE_TRG 0x20U /* Wake-up rising-edge triggers */
#define SYS_WKP_FALL_TRG 0x24U /* Wake-up falling-edge triggers */
#define SYS_WKP_PENDING  0x28U /* Wake-up pending (write-1-clear) */

/* RTC alarm -> SYS wake-up line 8 (SYS_WAKEUP_ALARM_ID, AltaRiscv.h). */
#define SYS_WKP_ALARM_ID  8U

/* RTC0 CRH alarm-interrupt-enable bit (backup-domain RTC @0x40000000). */
#define RTC0_CRH_ALRIE    BIT(1)

/* PWR_CNTL low-power mode field (SYS_LOWPOWER_MODE_MASK, system.h). */
#define PWR_LP_MODE_MASK 0x3U
#define PWR_LP_SLEEP     0x0U  /* CPU clock off; peripherals + CLINT run */
#define PWR_LP_STOP      0x1U  /* all clocks stopped */
#define PWR_LP_STANDBY   0x3U  /* 1.2 V domain off */

/* RST_CNTL software reset bit (SYS_RST_SFT, system.h). */
#define RST_SOFT_RESET  BIT(0)

/* DBG_CNTL stop/standby freeze bits (SYS_DBG_STOP / SYS_DBG_STANDBY).
 * Cleared before deep entry — the SDK does the same
 * (SYS_DisableDebugConfig(SYS_DBG_ALL)) so an attached probe cannot
 * hold the core out of stop/standby. */
#define DBG_DEEP_MASK   (BIT(1) | BIT(2))

extern void agrv2k_clk_switch_hsi(void);
extern void agrv2k_clk_switch_pll(uint32_t pll_hz, uint32_t flash_max_hz);

static inline uint32_t sys_read(uint32_t off)
{
	return *((volatile uint32_t *)(SYS_BASE + off));
}

static inline void sys_write(uint32_t off, uint32_t val)
{
	*((volatile uint32_t *)(SYS_BASE + off)) = val;
}

/* PLL target after stop-mode wake — the same DT properties soc.c reads
 * for boot-time clock setup (sys-controller@3000000). */
static const uint32_t pm_pll_freq =
	DT_PROP_BY_PHANDLE(DT_NODELABEL(sys), clocks, clock_frequency);
static const uint32_t pm_flash_max_freq =
	DT_PROP(DT_NODELABEL(sys), flash_max_frequency);

static inline void sys_set_lowpower_mode(uint32_t mode)
{
	sys_write(SYS_PWR_CNTL,
		  (sys_read(SYS_PWR_CNTL) & ~PWR_LP_MODE_MASK) | mode);
	/* Make sure the PWR_CNTL write is effective before WFI (the SDK
	 * does the same in SYS_EnterSleepMode/StopMode/StandbyMode). */
	__asm__ volatile("fence" ::: "memory");
}

/* True when a stop/standby entry has a real wake source: either the
 * application pre-armed one of the EXT_INTx wake edges (WKP_RISE/FALL
 * trigger bits 0-7) or an RTC alarm is armed through the counter API
 * (rtc0 CRH.ALRIE set by the driver's set_alarm). Without one of the
 * two, stop locks the SoC until power-on reset. */
static bool agrv2k_deep_wake_armed(void)
{
	if ((sys_read(SYS_WKP_RISE_TRG) | sys_read(SYS_WKP_FALL_TRG)) != 0U) {
		return true;
	}

	if (DT_NODE_HAS_STATUS(DT_NODELABEL(rtc0), okay)) {
		volatile uint16_t *crh =
			(volatile uint16_t *)(DT_REG_ADDR(DT_NODELABEL(rtc0)) +
					      0x00U);

		return (*crh & RTC0_CRH_ALRIE) != 0U;
	}

	return false;
}

/* Mirror an armed RTC alarm onto the SYS wake-up controller: WKP line 8
 * (SYS_WAKEUP_ALARM_ID), rising edge — SDK
 * SYS_SetAlarmWakeupEdge(SYS_WAKEUP_EDGE_RISE). Clears any stale
 * pending wake-up first so the just-armed alarm is the one that wakes
 * the SoC. */
static void agrv2k_arm_rtc_wake(void)
{
	sys_write(SYS_WKP_RISE_TRG,
		  sys_read(SYS_WKP_RISE_TRG) | BIT(SYS_WKP_ALARM_ID));
	sys_write(SYS_WKP_FALL_TRG,
		  sys_read(SYS_WKP_FALL_TRG) & ~BIT(SYS_WKP_ALARM_ID));
	sys_write(SYS_WKP_PENDING, sys_read(SYS_WKP_PENDING));
	__asm__ volatile("fence" ::: "memory");
}

void pm_state_set(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(substate_id);
	bool wake_armed;

	switch (state) {
	case PM_STATE_SUSPEND_TO_IDLE:
		sys_set_lowpower_mode(PWR_LP_SLEEP);
		break;
	case PM_STATE_STANDBY:
		wake_armed = agrv2k_deep_wake_armed();
		if (!wake_armed) {
			LOG_WRN("STANDBY: no wake source (RTC alarm or EXT_INTx "
				"edge); sleeping instead of stop");
			sys_set_lowpower_mode(PWR_LP_SLEEP);
			break;
		}
		/* Only mirror the alarm when the app did not pre-arm an
		 * EXT_INTx edge itself. */
		if (sys_read(SYS_WKP_RISE_TRG) == 0U &&
		    sys_read(SYS_WKP_FALL_TRG) == 0U) {
			agrv2k_arm_rtc_wake();
		}
		sys_set_lowpower_mode(PWR_LP_STOP);
		sys_write(SYS_DBG_CNTL, sys_read(SYS_DBG_CNTL) & ~DBG_DEEP_MASK);
		agrv2k_clk_switch_hsi();
		break;
	case PM_STATE_SOFT_OFF:
		wake_armed = agrv2k_deep_wake_armed();
		if (!wake_armed) {
			LOG_WRN("SOFT_OFF: no wake source (RTC alarm or "
				"EXT_INTx edge); sleeping instead of standby");
			sys_set_lowpower_mode(PWR_LP_SLEEP);
			break;
		}
		if (sys_read(SYS_WKP_RISE_TRG) == 0U &&
		    sys_read(SYS_WKP_FALL_TRG) == 0U) {
			agrv2k_arm_rtc_wake();
		}
		sys_set_lowpower_mode(PWR_LP_STANDBY);
		sys_write(SYS_DBG_CNTL, sys_read(SYS_DBG_CNTL) & ~DBG_DEEP_MASK);
		agrv2k_clk_switch_hsi();
		break;
	default:
		/* Not a state this SoC maps (e.g. runtime-idle): fall back
		 * to plain WFI, which is the no-PM idle behavior. */
		break;
	}

	/* Ensure the PWR_CNTL / CLK_CNTL writes above are visible before
	 * the core waits (SDK does the same between PWR_CNTL and WFI). */
	__asm__ volatile("fence" ::: "memory");

	__asm__ volatile("wfi");

	if (state == PM_STATE_SOFT_OFF) {
		/* With the FLASH option byte STDBY_NO_RST set, the RTC
		 * alarm resumes the core here instead of a power-on reset;
		 * force a software reset so the image always restarts
		 * from a known state (SYS_RSTF_SFT at the next boot). */
		sys_write(SYS_RST_CNTL, sys_read(SYS_RST_CNTL) | RST_SOFT_RESET);
		for (;;) {
		}
	}
}

void pm_state_exit_post_ops(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(substate_id);

	/* Runs with interrupts still locked, before sys_clock_idle_exit().
	 * sleep mode needs nothing (CLINT kept running); soft-off never
	 * returns here. */
	if (state == PM_STATE_STANDBY) {
		/* Restore PLL + FLASH divider — HSI has been the source
		 * since before WFI (see pm_state_set above). */
		agrv2k_clk_switch_pll(pm_pll_freq, pm_flash_max_freq);
		/* Drop the RTC-alarm wake-up latch the SDK clears after
		 * every stop exit (SYS_ClearPendingWakeups). */
		sys_write(SYS_WKP_PENDING, sys_read(SYS_WKP_PENDING));
	}
}

#endif /* CONFIG_PM */
