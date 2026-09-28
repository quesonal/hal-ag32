/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AGM AgRV2K RTC counter driver.
 *
 * Hardware: STM32F1-style RTC in the backup domain at 0x40000000
 * (SDK framework-agrv_sdk/src/rtc.h). All registers are 16-bit
 * halfwords at 4-byte stride, written through a 32-bit bus:
 *   CRH  0x00  interrupt enable: SECIE b0 / ALRIE b1 / OWIE b2
 *   CRL  0x04  flags: SECF b0 / ALRF b1 / OWF b2 / RSF b3 / RTOFF b5
 *   PRLH 0x08  prescaler reload [19:16] (only 4 bits used)
 *   PRLL 0x0c  prescaler reload [15:0]
 *   DIVH 0x10 / DIVL 0x14  down-counter (read only)
 *   CNTH 0x18 / CNTL 0x1c  32-bit up counter
 *   ALRH 0x20 / ALRL 0x24  32-bit alarm value
 *   BDCR 0x30  LSEON b0 / LSERDY b1 / LSEBYP b2 / RTCSEL b9:8 /
 *              RTCEN b15
 *   BDRST 0x32 backup-domain software reset (b0)
 *
 * Any write to the RTC/backup-domain registers must be followed by a
 * wait for CRL.RTOFF (SDK RTC_WRITE_REG / RTC_WaitForWrite); alarm
 * flags are cleared only after DIV has moved past PRL (SDK
 * RTC_ClearInt). The counter increments every (PRL + 1) RTC clock
 * ticks, i.e. 1 Hz with the DT defaults (32768 Hz LSE, PRL 32767) —
 * SDk examples load PRL with the RTC clock rate for the same ~1 s
 * cadence. The RTC is in the backup domain: there is no APB clock
 * gate and it keeps running across SoC resets, so the driver only
 * resets the counter on the first power-on (BDCR.RTCEN == 0) and
 * preserves the value on warm resets.
 *
 * Zephyr model: one counter device, single alarm channel (the alarm
 * register; ALRF is set when CNT reaches ALR) plus an optional top
 * callback on the 32-bit counter wrap (OWF).
 */

#define DT_DRV_COMPAT agm_agrv2k_rtc

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

LOG_MODULE_REGISTER(counter_agm_rtc, CONFIG_COUNTER_LOG_LEVEL);

/* The register map is public: the bootloader's one-shot override and the
 * trial-boot handshake talk to the same backup domain directly (see the
 * header for why). */
#include <zephyr/drivers/counter/agm_rtc_regs.h>

#define AGM_RTC_TOP		UINT32_MAX
#define AGM_RTC_NUM_CH		1U

/* Bounded spin for RTOFF / DIV-sync / LSERDY waits. */
#define AGM_RTC_WAIT_SPINS	100000U

/* LSE start-up attempts. Each round waits AGM_RTC_WAIT_SPINS * 20
 * iterations (~1 s on this core) and re-asserts LSEON.
 * A cold (backup-domain-reset) boot can need well over one second;
 * an absent crystal still fails after ~4 s instead of hanging.
 */
#define AGM_RTC_LSE_ROUNDS	4U

enum agm_rtc_source {
	AGM_RTC_SOURCE_LSE,
	AGM_RTC_SOURCE_LSI,
	AGM_RTC_SOURCE_LOCAL,
};

struct agm_rtc_data {
	struct k_spinlock lock;
	counter_alarm_callback_t callback; /* alarm channel 0 */
	void *user_data;
	uint32_t target; /* absolute tick of the armed alarm */
	counter_top_callback_t top_callback;
	void *top_user_data;
	bool running;
};

struct agm_rtc_cfg {
	struct counter_config_info info;
	uint32_t base;
	uint32_t clk_hz;	/* rtc clock frequency (agm,rtc-frequency) */
	uint32_t prescaler;	/* PRL reload value */
	enum agm_rtc_source source;
	void (*irq_config)(void);
};

static inline uint16_t agm_rtc_read16(const struct agm_rtc_cfg *cfg,
				      uint32_t reg)
{
	return *(volatile uint16_t *)(cfg->base + reg);
}

static inline void agm_rtc_write16(const struct agm_rtc_cfg *cfg,
				   uint32_t reg, uint16_t val)
{
	*(volatile uint16_t *)(cfg->base + reg) = val;
}

static inline uint32_t agm_rtc_read32(const struct agm_rtc_cfg *cfg,
				      uint32_t reg)
{
	return ((uint32_t)agm_rtc_read16(cfg, reg) << 16) |
	       agm_rtc_read16(cfg, reg + 4U);
}

static void agm_rtc_wait_rtoff(const struct agm_rtc_cfg *cfg)
{
	uint32_t spins = AGM_RTC_WAIT_SPINS;

	while ((agm_rtc_read16(cfg, AGM_RTC_CRL) & AGM_RTC_CRL_RTOFF) == 0U) {
		if (--spins == 0U) {
			LOG_ERR("RTC write-sync timeout (RTOFF)");
			break;
		}
	}
}

/* Write a register and wait for the backup-domain write sync (SDK
 * RTC_WRITE_REG). */
static void agm_rtc_write_wait(const struct agm_rtc_cfg *cfg, uint32_t reg,
			       uint16_t val)
{
	agm_rtc_write16(cfg, reg, val);
	agm_rtc_wait_rtoff(cfg);
}

/* Clear interrupt flags only once the divider has moved past the
 * prescaler reload (SDK RTC_ClearInt) so a just-completed counter
 * increment is not lost. */
static void agm_rtc_clear_int(const struct agm_rtc_cfg *cfg, uint16_t bits)
{
	uint32_t spins = AGM_RTC_WAIT_SPINS;

	while (agm_rtc_read32(cfg, AGM_RTC_DIVH) ==
	       agm_rtc_read32(cfg, AGM_RTC_PRLH)) {
		if (--spins == 0U) {
			break;
		}
	}
	agm_rtc_write16(cfg, AGM_RTC_CRL,
			(uint16_t)(agm_rtc_read16(cfg, AGM_RTC_CRL) & ~bits));
}

static uint32_t agm_rtc_get_counter(const struct agm_rtc_cfg *cfg)
{
	uint16_t hi, lo, hi2;

	/* Retry on a torn 16-bit carry between CNTH and CNTL. */
	do {
		hi = agm_rtc_read16(cfg, AGM_RTC_CNTH);
		lo = agm_rtc_read16(cfg, AGM_RTC_CNTL);
		hi2 = agm_rtc_read16(cfg, AGM_RTC_CNTH);
	} while (hi != hi2);

	return ((uint32_t)hi << 16) | lo;
}

static void agm_rtc_set_counter(const struct agm_rtc_cfg *cfg, uint32_t value)
{
	agm_rtc_write_wait(cfg, AGM_RTC_CNTH, (uint16_t)(value >> 16));
	agm_rtc_write_wait(cfg, AGM_RTC_CNTL, (uint16_t)value);
}

static void agm_rtc_set_prescaler(const struct agm_rtc_cfg *cfg,
				  uint32_t prescaler)
{
	agm_rtc_write_wait(cfg, AGM_RTC_PRLH, (uint16_t)(prescaler >> 16));
	agm_rtc_write_wait(cfg, AGM_RTC_PRLL, (uint16_t)prescaler);
}

static void agm_rtc_program_alarm(const struct agm_rtc_cfg *cfg, uint32_t value)
{
	agm_rtc_write_wait(cfg, AGM_RTC_ALRH, (uint16_t)(value >> 16));
	agm_rtc_write_wait(cfg, AGM_RTC_ALRL, (uint16_t)value);
}

static uint16_t agm_rtc_irq_enable_bits(const struct agm_rtc_data *data)
{
	uint16_t bits = 0U;

	if (data->callback != NULL) {
		bits |= AGM_RTC_CRH_ALRIE;
	}
	if (data->top_callback != NULL) {
		bits |= AGM_RTC_CRH_OWIE;
	}
	return bits;
}

static int agm_rtc_start(const struct device *dev)
{
	const struct agm_rtc_cfg *cfg = dev->config;
	struct agm_rtc_data *data = dev->data;
	k_spinlock_key_t key;
	uint16_t crh;

	key = k_spin_lock(&data->lock);

	crh = agm_rtc_read16(cfg, AGM_RTC_CRH);
	crh = (uint16_t)((crh & ~(AGM_RTC_CRH_ALRIE | AGM_RTC_CRH_OWIE)) |
			 agm_rtc_irq_enable_bits(data));
	agm_rtc_write16(cfg, AGM_RTC_CRH, crh);
	data->running = true;

	k_spin_unlock(&data->lock, key);
	return 0;
}

static int agm_rtc_stop(const struct device *dev)
{
	const struct agm_rtc_cfg *cfg = dev->config;
	struct agm_rtc_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);

	agm_rtc_write16(cfg, AGM_RTC_CRH,
			(uint16_t)(agm_rtc_read16(cfg, AGM_RTC_CRH) &
				   ~(AGM_RTC_CRH_ALRIE | AGM_RTC_CRH_OWIE)));
	data->running = false;

	k_spin_unlock(&data->lock, key);
	return 0;
}

static int agm_rtc_get_value(const struct device *dev, uint32_t *ticks)
{
	const struct agm_rtc_cfg *cfg = dev->config;

	if (ticks == NULL) {
		return -EINVAL;
	}
	*ticks = agm_rtc_get_counter(cfg);
	return 0;
}

static int agm_rtc_set_alarm(const struct device *dev, uint8_t chan_id,
			     const struct counter_alarm_cfg *alarm_cfg)
{
	const struct agm_rtc_cfg *cfg = dev->config;
	struct agm_rtc_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t now, target;

	if (chan_id >= AGM_RTC_NUM_CH || alarm_cfg == NULL ||
	    alarm_cfg->callback == NULL) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);

	if (data->callback != NULL) {
		k_spin_unlock(&data->lock, key);
		return -EBUSY;
	}

	now = agm_rtc_get_counter(cfg);
	if (alarm_cfg->flags & COUNTER_ALARM_CFG_ABSOLUTE) {
		target = alarm_cfg->ticks;
	} else {
		target = now + alarm_cfg->ticks;
	}
	if (target == now) {
		/* An alarm equal to the current value would already have
		 * passed the compare; fire on the next increment. */
		target = now + 1U;
	}

	data->callback = alarm_cfg->callback;
	data->user_data = alarm_cfg->user_data;
	data->target = target;

	/* The RTC compare fires one tick after the programmed ALR value
	 * (observed on-target: ALRF at CNT == ALR + 1), so back off the
	 * register value by one to trigger at the requested target. */
	agm_rtc_program_alarm(cfg, target - 1U);
	/* Drop any stale ALRF before arming the compare. */
	agm_rtc_clear_int(cfg, AGM_RTC_CRL_ALRF);
	agm_rtc_write16(cfg, AGM_RTC_CRH,
			(uint16_t)(agm_rtc_read16(cfg, AGM_RTC_CRH) |
				   AGM_RTC_CRH_ALRIE));

	k_spin_unlock(&data->lock, key);
	return 0;
}

static int agm_rtc_cancel_alarm(const struct device *dev, uint8_t chan_id)
{
	const struct agm_rtc_cfg *cfg = dev->config;
	struct agm_rtc_data *data = dev->data;
	k_spinlock_key_t key;

	if (chan_id >= AGM_RTC_NUM_CH) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);

	if (data->callback == NULL) {
		k_spin_unlock(&data->lock, key);
		return 0;
	}

	agm_rtc_write16(cfg, AGM_RTC_CRH,
			(uint16_t)(agm_rtc_read16(cfg, AGM_RTC_CRH) &
				   ~AGM_RTC_CRH_ALRIE));
	agm_rtc_clear_int(cfg, AGM_RTC_CRL_ALRF);
	data->callback = NULL;
	data->target = 0U;

	k_spin_unlock(&data->lock, key);
	return 0;
}

static int agm_rtc_set_top_value(const struct device *dev,
				 const struct counter_top_cfg *top_cfg)
{
	const struct agm_rtc_cfg *cfg = dev->config;
	struct agm_rtc_data *data = dev->data;
	k_spinlock_key_t key;

	if (top_cfg == NULL || top_cfg->ticks != AGM_RTC_TOP) {
		return -ENOTSUP;
	}

	key = k_spin_lock(&data->lock);

	data->top_callback = top_cfg->callback;
	data->top_user_data = top_cfg->user_data;

	if (data->running) {
		uint16_t crh = agm_rtc_read16(cfg, AGM_RTC_CRH);

		if (top_cfg->callback != NULL) {
			agm_rtc_clear_int(cfg, AGM_RTC_CRL_OWF);
			crh |= AGM_RTC_CRH_OWIE;
		} else {
			crh &= ~AGM_RTC_CRH_OWIE;
		}
		agm_rtc_write16(cfg, AGM_RTC_CRH, crh);
	}

	k_spin_unlock(&data->lock, key);
	return 0;
}

static uint32_t agm_rtc_get_pending_int(const struct device *dev)
{
	const struct agm_rtc_cfg *cfg = dev->config;

	return agm_rtc_read16(cfg, AGM_RTC_CRL) &
	       (AGM_RTC_CRL_ALRF | AGM_RTC_CRL_OWF);
}

static uint32_t agm_rtc_get_top_value(const struct device *dev)
{
	return AGM_RTC_TOP;
}

static uint32_t agm_rtc_get_freq(const struct device *dev)
{
	const struct agm_rtc_cfg *cfg = dev->config;

	return cfg->info.freq;
}

/* Zephyr's ISR type -- see counter_agm_gptimer.c / uart_agm.c for why. */
static void agm_rtc_isr(const void *isr_arg)
{
	const struct device *dev = isr_arg;
	const struct agm_rtc_cfg *cfg = dev->config;
	struct agm_rtc_data *data = dev->data;
	counter_alarm_callback_t alarm_cb = NULL;
	counter_top_callback_t top_cb = NULL;
	void *alarm_user = NULL;
	void *top_user = NULL;
	uint32_t ticks = 0U;
	uint16_t crl, crh;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);

	crl = agm_rtc_read16(cfg, AGM_RTC_CRL);
	crh = agm_rtc_read16(cfg, AGM_RTC_CRH);

	if ((crl & AGM_RTC_CRL_ALRF) && (crh & AGM_RTC_CRH_ALRIE) &&
	    data->callback != NULL) {
		agm_rtc_clear_int(cfg, AGM_RTC_CRL_ALRF);
		alarm_cb = data->callback;
		alarm_user = data->user_data;
		ticks = data->target;
		data->callback = NULL;
		data->target = 0U;
	}
	if ((crl & AGM_RTC_CRL_OWF) && (crh & AGM_RTC_CRH_OWIE) &&
	    data->top_callback != NULL) {
		agm_rtc_clear_int(cfg, AGM_RTC_CRL_OWF);
		top_cb = data->top_callback;
		top_user = data->top_user_data;
	}

	k_spin_unlock(&data->lock, key);

	if (alarm_cb != NULL) {
		alarm_cb(dev, 0U, ticks, alarm_user);
	}
	if (top_cb != NULL) {
		top_cb(dev, top_user);
	}
}

static int agm_rtc_init(const struct device *dev)
{
	const struct agm_rtc_cfg *cfg = dev->config;
	uint16_t bdcr;
	bool fresh;
	uint32_t spins;

	/* Full backup-domain reset only on first power-on (RTCEN clear);
	 * on warm resets the RTC keeps counting and we preserve CNT. */
	fresh = (agm_rtc_read16(cfg, AGM_RTC_BDCR) & AGM_RTC_BDCR_RTCEN) == 0U;
	if (fresh) {
		/* SDK RTC_Reset(): reset backup domain then core regs. */
		agm_rtc_write16(cfg, AGM_RTC_BDRST, 1U);
		agm_rtc_write16(cfg, AGM_RTC_BDRST, 0U);
		agm_rtc_write16(cfg, AGM_RTC_CRH, 0U);
		agm_rtc_write16(cfg, AGM_RTC_CRL, 0U);
	}

	/* Select the clock source and enable the RTC (SDK RTC_Init). */
	bdcr = AGM_RTC_BDCR_RTCEN;
	switch (cfg->source) {
	case AGM_RTC_SOURCE_LSI:
		bdcr |= AGM_RTC_SRC_LSI;
		break;
	case AGM_RTC_SOURCE_LOCAL:
		bdcr |= AGM_RTC_SRC_LOCAL;
		break;
	case AGM_RTC_SOURCE_LSE:
	default:
		bdcr |= AGM_RTC_SRC_LSE | AGM_RTC_BDCR_LSEON;
		break;
	}
	agm_rtc_write_wait(cfg, AGM_RTC_BDCR, bdcr);

	if (cfg->source == AGM_RTC_SOURCE_LSE) {
		/* The crystal only starts once LSEON is set after a
		 * backup-domain reset, and 32 kHz crystals with high ESR can
		 * take several seconds to raise LSERDY. A single ~1 s budget
		 * made device_is_ready() spuriously false on the first boot
		 * of a freshly programmed image ("rtc counter not ready",
		 * samples/rtc_alarm).
		 *
		 * Wait in rounds, re-asserting LSEON each time. On a warm
		 * boot LSERDY is already set, so the loop body never runs.
		 * LSERDY is a sticky backup-domain status bit, so an early
		 * success in any round is not lost.
		 *
		 * No LSI fallback: RTCSEL is write-once (only writable after
		 * a backup-domain reset), so switching source here would need
		 * BDRST and would drop the running counter. A crystal that
		 * never starts is a board fault and stays visible.
		 */
		for (uint32_t round = 0U; round < AGM_RTC_LSE_ROUNDS; round++) {
			agm_rtc_write16(cfg, AGM_RTC_BDCR, bdcr);
			spins = AGM_RTC_WAIT_SPINS * 20U; /* ~1 s per round */
			while ((agm_rtc_read16(cfg, AGM_RTC_BDCR) &
				AGM_RTC_BDCR_LSERDY) == 0U) {
				if (--spins == 0U) {
					break;
				}
			}
			if ((agm_rtc_read16(cfg, AGM_RTC_BDCR) &
			     AGM_RTC_BDCR_LSERDY) != 0U) {
				break;
			}
			LOG_WRN("LSE not ready (LSERDY), retry %u/%u",
				round + 1U, AGM_RTC_LSE_ROUNDS);
		}
		if ((agm_rtc_read16(cfg, AGM_RTC_BDCR) &
		     AGM_RTC_BDCR_LSERDY) == 0U) {
			LOG_ERR("LSE not ready (LSERDY) after %u rounds; check "
				"the board crystal or set agm,rtc-source",
				AGM_RTC_LSE_ROUNDS);
			return -ETIMEDOUT;
		}
	}

	agm_rtc_set_prescaler(cfg, cfg->prescaler);
	if (fresh) {
		agm_rtc_set_counter(cfg, 0U);
	}
	/* Drop stale flags. */
	agm_rtc_clear_int(cfg, AGM_RTC_CRL_ALRF | AGM_RTC_CRL_OWF |
			  AGM_RTC_CRL_SECF);

	cfg->irq_config();
	LOG_INF("AGM RTC initialized (%s, %u Hz, PRL %u -> ~%u Hz counter%s)",
		cfg->source == AGM_RTC_SOURCE_LSE ? "lse" :
		cfg->source == AGM_RTC_SOURCE_LSI ? "lsi" : "local",
		cfg->clk_hz, cfg->prescaler, cfg->info.freq,
		fresh ? ", counter reset" : ", counter preserved");
	return 0;
}

static DEVICE_API(counter, agm_rtc_api) = {
	.start = agm_rtc_start,
	.stop = agm_rtc_stop,
	.get_value = agm_rtc_get_value,
	.set_alarm = agm_rtc_set_alarm,
	.cancel_alarm = agm_rtc_cancel_alarm,
	.set_top_value = agm_rtc_set_top_value,
	.get_pending_int = agm_rtc_get_pending_int,
	.get_top_value = agm_rtc_get_top_value,
	.get_freq = agm_rtc_get_freq,
};

#define AGM_RTC_IRQ_CONNECT(n)						\
	static void agm_rtc_irq_config_##n(void)			\
	{								\
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority),	\
			    agm_rtc_isr, DEVICE_DT_INST_GET(n), 0);	\
		irq_enable(DT_INST_IRQN(n));				\
	}

#define AGM_RTC_INIT(n)							\
	AGM_RTC_IRQ_CONNECT(n)						\
	static const struct agm_rtc_cfg agm_rtc_cfg_##n = {		\
		.info = {						\
			.max_top_value = AGM_RTC_TOP,			\
			.freq = DT_INST_PROP(n, agm_rtc_frequency) /	\
				(DT_INST_PROP(n, agm_prescaler) + 1U),	\
			.flags = COUNTER_CONFIG_INFO_COUNT_UP,		\
			.channels = AGM_RTC_NUM_CH,			\
		},							\
		.base = DT_INST_REG_ADDR(n),				\
		.clk_hz = DT_INST_PROP(n, agm_rtc_frequency),		\
		.prescaler = DT_INST_PROP(n, agm_prescaler),		\
		.source = (enum agm_rtc_source)DT_INST_ENUM_IDX(n, agm_rtc_source),\
		.irq_config = agm_rtc_irq_config_##n,			\
	};								\
	static struct agm_rtc_data agm_rtc_data_##n;			\
	DEVICE_DT_INST_DEFINE(n, agm_rtc_init, NULL, &agm_rtc_data_##n,	\
			      &agm_rtc_cfg_##n, POST_KERNEL,		\
			      CONFIG_COUNTER_INIT_PRIORITY, &agm_rtc_api);

DT_INST_FOREACH_STATUS_OKAY(AGM_RTC_INIT)
