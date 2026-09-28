/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AGM AgRV2K general-purpose timer (GPTIMER0-4) counter driver.
 *
 * Hardware: STM32-style general purpose timer (SDK gptimer.h): one
 * up-counting counter (CNT) with auto-reload (ARR), prescaler (PSC)
 * and four capture/compare channels. Register map:
 *   CR1 0x00 / CR2 0x04 / SMCR 0x08 / DIER 0x0C / SR 0x10 / EGR 0x14
 *   CCMR0/1 0x18/0x1C / CCER 0x20 / CNT 0x24 / PSC 0x28 / ARR 0x2C
 *   RCR 0x30 / CCR0-3 0x34..0x40 / BDTR 0x44
 * Instances GPTIMER0..4 @ 0x40020000..0x40024000 with PLIC IRQs
 * 19..23 and APB clock gate bits 16..20 (AltaRiscv.h
 * APB_MASK_GPTIMERx / GPTIMERx_IRQn). The counter runs at pclk == SYSCLK,
 * taken from the node's `clocks` phandle (clk0 = per-board AGM_SYSCLK_HZ;
 * the shipped 407 bitstream runs it at 200 MHz).
 *
 * Zephyr model: one counter device per gptimer with a single alarm
 * channel (compare channel 0) and an optional top callback on the
 * counter wrap. The counter free-runs from 0 to 0xFFFFFFFF and wraps
 * (update event) every 2^32 ticks; the value is CNT itself. Alarms
 * use compare channel 0 in output-compare toggle mode: CC0IF is set
 * when CNT == CCR0, so arming CCR0 with the absolute target tick
 * yields one interrupt at the requested counter value. PWM / input
 * capture / channels 1-3 are not wired up yet.
 *
 * Prescaler: PSC (0x28) comes from the node's `prescaler` property
 * (default 0). The tick rate is pclk / (prescaler + 1); both get_freq()
 * and counter_config_info.freq report that divided value.
 *
 * Not implemented:
 *   - ETR external trigger, with its SMCR.ETPS trigger prescaler
 *     (SDK GPTIMER_ETR_PrescalerTypeDef, /1../8).
 *   - Input capture, with its per-channel CCMR.ICxPSC prescaler
 *     (SDK GPTIMER_IC_PrescalerTypeDef, /1../8).
 *   - Channels 1-3; only compare channel 0 backs the alarm.
 *   - One-pulse mode (CR1.OPM) and repetition count (RCR).
 */

#define DT_DRV_COMPAT agm_agrv2k_gptimer

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/sys_io.h>

#include <agm_sys.h>

LOG_MODULE_REGISTER(counter_agm_gptimer, CONFIG_COUNTER_LOG_LEVEL);

/* SYS controller APB clock gate + GPTIMER base come from agm_sys.h
 *.
 */

/* GPTIMER register offsets (SDK gptimer.h). */
#define AGM_GPT_CR1		0x00U
#define AGM_GPT_DIER		0x0cU
#define AGM_GPT_SR		0x10U
#define AGM_GPT_EGR		0x14U
#define AGM_GPT_CCMR0		0x18U
#define AGM_GPT_CCER		0x20U
#define AGM_GPT_CNT		0x24U
#define AGM_GPT_PSC		0x28U
#define AGM_GPT_ARR		0x2cU
#define AGM_GPT_CCR0		0x34U

#define AGM_GPT_CR1_CEN		BIT(0)

#define AGM_GPT_DIER_UIE	BIT(0)
#define AGM_GPT_DIER_CC0IE	BIT(1)

#define AGM_GPT_SR_UIF		BIT(0)
#define AGM_GPT_SR_CC0IF	BIT(1)

#define AGM_GPT_EGR_UG		BIT(0)

#define AGM_GPT_CCMR0_OC0M	(7U << 4)
#define AGM_GPT_OCM_TOGGLE	(3U << 4) /* OC0REF toggles at CNT == CCR0 */

#define AGM_GPT_CCER_CC0E	BIT(0)

#define AGM_GPT_TOP		UINT32_MAX

/* Tick rate = pclk / (PSC + 1). Kept as a macro so the constant
 * counter_config_info.freq initializer below stays a compile-time
 * expression, matching what Zephyr expects there.
 */
#define AGM_GPT_TICK_HZ(pclk_hz, psc)	((pclk_hz) / ((psc) + 1U))

struct agm_gpt_data {
	struct k_spinlock lock;
	counter_alarm_callback_t callback;
	void *user_data;
	uint32_t target;
	counter_top_callback_t top_callback;
	void *top_user_data;
	bool running;
};

struct agm_gpt_cfg {
	struct counter_config_info info;
	uint32_t base;
	uint32_t pclk_hz;
	uint32_t prescaler;
	void (*irq_config)(void);
};

static inline uint32_t agm_gpt_read(const struct agm_gpt_cfg *cfg,
				    uint32_t reg)
{
	return sys_read32(cfg->base + reg);
}

static inline void agm_gpt_write(const struct agm_gpt_cfg *cfg,
				 uint32_t reg, uint32_t val)
{
	sys_write32(val, cfg->base + reg);
}

static int agm_gpt_start(const struct device *dev)
{
	const struct agm_gpt_cfg *cfg = dev->config;
	struct agm_gpt_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t cr1;

	key = k_spin_lock(&data->lock);

	cr1 = agm_gpt_read(cfg, AGM_GPT_CR1);
	if ((cr1 & AGM_GPT_CR1_CEN) == 0U) {
		agm_gpt_write(cfg, AGM_GPT_CR1, cr1 | AGM_GPT_CR1_CEN);
	}
	data->running = true;

	k_spin_unlock(&data->lock, key);
	return 0;
}

static int agm_gpt_stop(const struct device *dev)
{
	const struct agm_gpt_cfg *cfg = dev->config;
	struct agm_gpt_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t dier;

	key = k_spin_lock(&data->lock);

	agm_gpt_write(cfg, AGM_GPT_CR1, agm_gpt_read(cfg, AGM_GPT_CR1) &
		      ~AGM_GPT_CR1_CEN);
	dier = agm_gpt_read(cfg, AGM_GPT_DIER) & ~AGM_GPT_DIER_CC0IE;
	agm_gpt_write(cfg, AGM_GPT_DIER, dier);
	data->callback = NULL;
	data->running = false;

	k_spin_unlock(&data->lock, key);
	return 0;
}

static int agm_gpt_get_value(const struct device *dev, uint32_t *ticks)
{
	const struct agm_gpt_cfg *cfg = dev->config;

	*ticks = agm_gpt_read(cfg, AGM_GPT_CNT);
	return 0;
}

static int agm_gpt_set_alarm(const struct device *dev, uint8_t chan_id,
			     const struct counter_alarm_cfg *alarm_cfg)
{
	const struct agm_gpt_cfg *cfg = dev->config;
	struct agm_gpt_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t now, target;

	if (chan_id != 0U || alarm_cfg == NULL ||
	    alarm_cfg->callback == NULL) {
		/* No upper bound on `ticks`: AGM_GPT_TOP is UINT32_MAX, i.e. the
		 * whole 32-bit range is representable and the old
		 * `ticks > AGM_GPT_TOP` test could never fire. Absolute targets are
		 * compared modulo 2^32 below. */
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);

	if (!data->running) {
		k_spin_unlock(&data->lock, key);
		return -EIO;
	}
	if (data->callback != NULL) {
		k_spin_unlock(&data->lock, key);
		return -EBUSY;
	}

	now = agm_gpt_read(cfg, AGM_GPT_CNT);
	if (alarm_cfg->flags & COUNTER_ALARM_CFG_ABSOLUTE) {
		target = alarm_cfg->ticks;
	} else {
		target = now + alarm_cfg->ticks;
	}
	if (target == now) {
		/* Alarm already due: expire on the very next tick. */
		target = now + 1U;
	}

	/* Compare channel 0 fires CC0IF when CNT == CCR0. */
	agm_gpt_write(cfg, AGM_GPT_CCR0, target);
	agm_gpt_write(cfg, AGM_GPT_SR, ~AGM_GPT_SR_CC0IF);

	data->callback = alarm_cfg->callback;
	data->user_data = alarm_cfg->user_data;
	data->target = target;

	agm_gpt_write(cfg, AGM_GPT_DIER,
		      agm_gpt_read(cfg, AGM_GPT_DIER) | AGM_GPT_DIER_CC0IE);

	k_spin_unlock(&data->lock, key);
	return 0;
}

static int agm_gpt_cancel_alarm(const struct device *dev, uint8_t chan_id)
{
	const struct agm_gpt_cfg *cfg = dev->config;
	struct agm_gpt_data *data = dev->data;
	k_spinlock_key_t key;

	if (chan_id != 0U) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);

	agm_gpt_write(cfg, AGM_GPT_DIER,
		      agm_gpt_read(cfg, AGM_GPT_DIER) & ~AGM_GPT_DIER_CC0IE);
	agm_gpt_write(cfg, AGM_GPT_SR, ~AGM_GPT_SR_CC0IF);
	data->callback = NULL;

	k_spin_unlock(&data->lock, key);
	return 0;
}

static int agm_gpt_set_top_value(const struct device *dev,
				 const struct counter_top_cfg *cfg)
{
	const struct agm_gpt_cfg *hcfg = dev->config;
	struct agm_gpt_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t dier;

	if (cfg == NULL) {
		return -EINVAL;
	}
	if (cfg->ticks != AGM_GPT_TOP) {
		/* The 32-bit free-run top is fixed by the driver. */
		return -ENOTSUP;
	}

	key = k_spin_lock(&data->lock);

	if (data->callback != NULL) {
		k_spin_unlock(&data->lock, key);
		return -EBUSY;
	}

	data->top_callback = cfg->callback;
	data->top_user_data = cfg->user_data;

	dier = agm_gpt_read(hcfg, AGM_GPT_DIER) & ~AGM_GPT_DIER_UIE;
	if (cfg->callback != NULL) {
		/* Clear any stale wrap flag before enabling UIE. */
		agm_gpt_write(hcfg, AGM_GPT_SR, ~AGM_GPT_SR_UIF);
		dier |= AGM_GPT_DIER_UIE;
	}
	agm_gpt_write(hcfg, AGM_GPT_DIER, dier);

	k_spin_unlock(&data->lock, key);
	return 0;
}

static uint32_t agm_gpt_get_pending_int(const struct device *dev)
{
	const struct agm_gpt_cfg *cfg = dev->config;

	return agm_gpt_read(cfg, AGM_GPT_SR) &
	       (AGM_GPT_SR_UIF | AGM_GPT_SR_CC0IF);
}

static uint32_t agm_gpt_get_top_value(const struct device *dev)
{
	return AGM_GPT_TOP;
}

static uint32_t agm_gpt_get_freq(const struct device *dev)
{
	const struct agm_gpt_cfg *cfg = dev->config;

	return AGM_GPT_TICK_HZ(cfg->pclk_hz, cfg->prescaler);
}

/* Zephyr's ISR type (see uart_agm.c for the full reason): IRQ_CONNECT() pastes
 * __isr_ ## <handler> into the table symbol, so the handler must be a bare
 * identifier, and the native/POSIX arch is the only one whose
 * ARCH_IRQ_CONNECT() does not cast it itself -- a `const struct device *`
 * handler is -Wincompatible-pointer-types there. */
static void agm_gpt_isr(const void *isr_arg)
{
	const struct device *dev = isr_arg;
	const struct agm_gpt_cfg *cfg = dev->config;
	struct agm_gpt_data *data = dev->data;
	counter_alarm_callback_t alarm_cb = NULL;
	counter_top_callback_t top_cb = NULL;
	void *alarm_user = NULL;
	void *top_user = NULL;
	uint32_t ticks = 0U;
	uint32_t sr;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);

	sr = agm_gpt_read(cfg, AGM_GPT_SR);

	if ((sr & AGM_GPT_SR_CC0IF) != 0U) {
		agm_gpt_write(cfg, AGM_GPT_SR, ~AGM_GPT_SR_CC0IF);
		if (data->callback != NULL) {
			alarm_cb = data->callback;
			alarm_user = data->user_data;
			ticks = data->target;
			data->callback = NULL;
			agm_gpt_write(cfg, AGM_GPT_DIER,
				      agm_gpt_read(cfg, AGM_GPT_DIER) &
				      ~AGM_GPT_DIER_CC0IE);
		}
	}

	if ((sr & AGM_GPT_SR_UIF) != 0U) {
		agm_gpt_write(cfg, AGM_GPT_SR, ~AGM_GPT_SR_UIF);
		if (data->top_callback != NULL) {
			top_cb = data->top_callback;
			top_user = data->top_user_data;
		}
	}

	k_spin_unlock(&data->lock, key);

	if (alarm_cb != NULL) {
		alarm_cb(dev, 0U, ticks, alarm_user);
	}
	if (top_cb != NULL) {
		top_cb(dev, top_user);
	}
}

static int agm_gpt_init(const struct device *dev)
{
	const struct agm_gpt_cfg *cfg = dev->config;

	/* The instance's APB gate (agm,apb-clkenable-bit = APB_MASK_GPTIMERx)
	 * was opened by soc.c at PRE_KERNEL_1 from devicetree; this driver no
	 * longer derives the bit from its register address. */

	/* Counter stopped, free-run 0..0xFFFFFFFF, prescale /1. */
	agm_gpt_write(cfg, AGM_GPT_CR1, 0U);
	agm_gpt_write(cfg, AGM_GPT_PSC, cfg->prescaler);
	agm_gpt_write(cfg, AGM_GPT_ARR, AGM_GPT_TOP);
	agm_gpt_write(cfg, AGM_GPT_CNT, 0U);

	/* Compare channel 0 in output-compare toggle mode. The OC output
	 * is never routed to a pin (no AFSEL), so this only generates
	 * the CC0IF compare event used for alarms.
	 */
	agm_gpt_write(cfg, AGM_GPT_CCMR0, AGM_GPT_OCM_TOGGLE);
	agm_gpt_write(cfg, AGM_GPT_CCER, AGM_GPT_CCER_CC0E);
	agm_gpt_write(cfg, AGM_GPT_DIER, 0U);
	agm_gpt_write(cfg, AGM_GPT_EGR, AGM_GPT_EGR_UG); /* load ARR/PSC */
	agm_gpt_write(cfg, AGM_GPT_SR, ~(AGM_GPT_SR_UIF | AGM_GPT_SR_CC0IF));

	cfg->irq_config();
	LOG_INF("AGM gptimer initialized (pclk %u Hz)", cfg->pclk_hz);
	return 0;
}

static DEVICE_API(counter, agm_gpt_api) = {
	.start = agm_gpt_start,
	.stop = agm_gpt_stop,
	.get_value = agm_gpt_get_value,
	.set_alarm = agm_gpt_set_alarm,
	.cancel_alarm = agm_gpt_cancel_alarm,
	.set_top_value = agm_gpt_set_top_value,
	.get_pending_int = agm_gpt_get_pending_int,
	.get_top_value = agm_gpt_get_top_value,
	.get_freq = agm_gpt_get_freq,
};

#define AGM_GPT_IRQ_CONNECT(n)							\
	static void agm_gpt_irq_config_##n(void)				\
	{									\
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority),		\
			    agm_gpt_isr, DEVICE_DT_INST_GET(n), 0);		\
		irq_enable(DT_INST_IRQN(n));					\
	}

/* The APB gate bit is derived from the reg address, so assert the node
 * sits on one of the GPTIMER0..4 pages. Comparing against
 * AGM_GPTIMER_BASE(n) would be wrong: Zephyr renumbers DT instances so
 * status-okay nodes come first.
 */
#define AGM_GPT_INIT(n)								\
	BUILD_ASSERT(((DT_INST_REG_ADDR(n) - AGM_GPTIMER_BASE(0)) % AGM_APB_STRIDE) == 0 && \
		     ((DT_INST_REG_ADDR(n) - AGM_GPTIMER_BASE(0)) / AGM_APB_STRIDE) < 5U, \
		     "gptimer" #n " reg address is not one of the GPTIMER0..4 pages"); \
	AGM_GPT_IRQ_CONNECT(n)							\
	static const struct agm_gpt_cfg agm_gpt_cfg_##n = {			\
		.info = {							\
			.max_top_value = AGM_GPT_TOP,				\
			.freq = AGM_GPT_TICK_HZ(					\
				DT_INST_PROP_BY_PHANDLE(n, clocks, clock_frequency), \
				DT_INST_PROP(n, prescaler)),			\
			.flags = COUNTER_CONFIG_INFO_COUNT_UP,			\
			.channels = 1U,						\
		},								\
		.base = DT_INST_REG_ADDR(n),					\
		.pclk_hz = DT_INST_PROP_BY_PHANDLE(n, clocks, clock_frequency),			\
		.prescaler = DT_INST_PROP(n, prescaler),				\
		.irq_config = agm_gpt_irq_config_##n,				\
	};									\
	static struct agm_gpt_data agm_gpt_data_##n;				\
	DEVICE_DT_INST_DEFINE(n, agm_gpt_init, NULL, &agm_gpt_data_##n,		\
			      &agm_gpt_cfg_##n, POST_KERNEL,			\
			      CONFIG_COUNTER_INIT_PRIORITY, &agm_gpt_api);

DT_INST_FOREACH_STATUS_OKAY(AGM_GPT_INIT)
