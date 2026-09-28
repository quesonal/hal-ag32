/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AGM AgRV2K dual 32-bit timer (TIMER0/TIMER1) counter driver.
 *
 * Hardware: SP804-style dual timer unit. Each unit has two fully
 * independent 16/32-bit down-counting sub-timers (register sets at
 * +0x00 / +0x20) that share a single PLIC interrupt line:
 *   TIMER0 @ 0x4001E000, PLIC IRQ 17, APB gate bit 14
 *   TIMER1 @ 0x4001F000, PLIC IRQ 18, APB gate bit 15
 * Sub-timers count down at pclk == SYSCLK, from the node's `clocks` phandle
 * (clk0 = per-board AGM_SYSCLK_HZ; the shipped 407 bitstream runs it at
 * 200 MHz,
 * bitstream). The SDK names
 * the sub-timers
 * Timer1/Timer2
 * (timer.h TIM_Init1/2) and gates the APB clock with
 * APB_MASK_TIMER0/1.
 *
 * Zephyr model: one counter device per unit with two alarm channels
 * (channel 0 = sub-timer 1, channel 1 = sub-timer 2). The counter
 * value is a 32-bit count-up value wrapping every 2^32 ticks. It is
 * derived from sub-timer 1, which free-runs in periodic mode loaded
 * with 0xFFFFFFFF while no channel-0 alarm is pending:
 *
 *   value = phase + (0xFFFFFFFF - cnt)          (free-run)
 *   value = target - cnt                        (ch0 alarm armed)
 *
 * phase is the value at the moment sub-timer 1 was last (re)loaded
 * with the free-run top; target is the absolute tick of an armed
 * channel-0 alarm. A channel-0 alarm temporarily programs sub-timer 1
 * with the (short) countdown delta; the ISR restores free-run and
 * advances phase afterwards so the value stays monotonic. Channel-1
 * alarms use sub-timer 2 and never affect the reported value.
 *
 * Both sub-timers run in periodic mode with interrupt enable toggled
 * by the driver, so alarm expiration, wrap (top callback) and alarm
 * cancel are all handled by rewriting the load register while the
 * timer is disabled - deterministic regardless of how the core
 * reloads after a match.
 */

#define DT_DRV_COMPAT agm_agrv2k_timer

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/sys_io.h>

#include <agm_sys.h>

LOG_MODULE_REGISTER(counter_agm_timer, CONFIG_COUNTER_LOG_LEVEL);

/* SYS controller APB clock gate + TIMER0/1 base come from agm_sys.h
 *.
 */

/* Sub-timer register offsets (sub-timer stride 0x20; SDK timer.h). */
#define AGM_TIMER_LOAD		0x00U
#define AGM_TIMER_VALUE		0x04U
#define AGM_TIMER_CTRL		0x08U
#define AGM_TIMER_INTCLR	0x0cU
#define AGM_TIMER_RIS		0x10U
#define AGM_TIMER_MIS		0x14U
#define AGM_TIMER_BGL		0x18U
#define AGM_TIMER_SUB_STRIDE	0x20U

#define AGM_TIMER_INT_MSK	BIT(0)

/* Control bits (SDK timer.h TIMER_CTRL_*). */
#define AGM_TIMER_CTRL_ONESHOT	BIT(0)
#define AGM_TIMER_CTRL_SIZE	BIT(1)
#define AGM_TIMER_CTRL_INT_EN	BIT(5)
#define AGM_TIMER_CTRL_PERIODIC	BIT(6)
#define AGM_TIMER_CTRL_ENABLE	BIT(7)

/* 32-bit counter, prescale /1, periodic (auto-reload) free-run. */
#define AGM_TIMER_CTRL_FREE_RUN	(AGM_TIMER_CTRL_PERIODIC | AGM_TIMER_CTRL_SIZE)

#define AGM_TIMER_TOP		UINT32_MAX
#define AGM_TIMER_NUM_CH	2U

struct agm_timer_ch_data {
	counter_alarm_callback_t callback;
	void *user_data;
	uint32_t target; /* absolute tick of the pending alarm */
};

struct agm_timer_data {
	struct k_spinlock lock;
	struct agm_timer_ch_data ch[AGM_TIMER_NUM_CH];
	uint32_t phase; /* value at the last sub-timer-1 free-run load */
	counter_top_callback_t top_callback;
	void *top_user_data;
	bool running;
};

struct agm_timer_cfg {
	struct counter_config_info info;
	uint32_t base;
	uint32_t pclk_hz;
	void (*irq_config)(void);
};

static inline uint32_t agm_timer_sub_off(uint8_t sub, uint32_t reg)
{
	return (uint32_t)sub * AGM_TIMER_SUB_STRIDE + reg;
}

static inline uint32_t agm_timer_read(const struct agm_timer_cfg *cfg,
				      uint8_t sub, uint32_t reg)
{
	return sys_read32(cfg->base + agm_timer_sub_off(sub, reg));
}

static inline void agm_timer_write(const struct agm_timer_cfg *cfg,
				   uint8_t sub, uint32_t reg, uint32_t val)
{
	sys_write32(val, cfg->base + agm_timer_sub_off(sub, reg));
}

/* Stop a sub-timer and reset it to a disabled, interrupt-off state. */
static void agm_timer_sub_reset(const struct agm_timer_cfg *cfg, uint8_t sub)
{
	agm_timer_write(cfg, sub, AGM_TIMER_CTRL, 0U);
	agm_timer_write(cfg, sub, AGM_TIMER_INTCLR, AGM_TIMER_INT_MSK);
}

/* Load a sub-timer and leave it running (periodic, 32-bit, /1). */
static void agm_timer_sub_load(const struct agm_timer_cfg *cfg, uint8_t sub,
			       uint32_t load, bool int_en)
{
	agm_timer_write(cfg, sub, AGM_TIMER_CTRL, 0U);
	agm_timer_write(cfg, sub, AGM_TIMER_INTCLR, AGM_TIMER_INT_MSK);
	agm_timer_write(cfg, sub, AGM_TIMER_LOAD, load);
	agm_timer_write(cfg, sub, AGM_TIMER_BGL, load);
	agm_timer_write(cfg, sub, AGM_TIMER_CTRL,
			AGM_TIMER_CTRL_FREE_RUN |
			(int_en ? AGM_TIMER_CTRL_INT_EN : 0U) |
			AGM_TIMER_CTRL_ENABLE);
}

/* Current counter value. Must be called with the spinlock held. */
static uint32_t agm_timer_now(const struct agm_timer_cfg *cfg,
			      const struct agm_timer_data *data)
{
	uint32_t cnt = agm_timer_read(cfg, 0U, AGM_TIMER_VALUE);

	if (data->ch[0].callback != NULL) {
		return data->ch[0].target - cnt;
	}

	return data->phase + (AGM_TIMER_TOP - cnt);
}

static int agm_timer_start(const struct device *dev)
{
	const struct agm_timer_cfg *cfg = dev->config;
	struct agm_timer_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);

	if (!data->running) {
		/* Sub-timer 1 free-runs from value 0. */
		data->phase = 0U;
		agm_timer_sub_load(cfg, 0U, AGM_TIMER_TOP,
				   data->top_callback != NULL);
		agm_timer_sub_reset(cfg, 1U);
		data->running = true;
	}

	k_spin_unlock(&data->lock, key);
	return 0;
}

static int agm_timer_stop(const struct device *dev)
{
	const struct agm_timer_cfg *cfg = dev->config;
	struct agm_timer_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);

	agm_timer_sub_reset(cfg, 0U);
	agm_timer_sub_reset(cfg, 1U);
	data->ch[0].callback = NULL;
	data->ch[1].callback = NULL;
	data->phase = 0U;
	data->running = false;

	k_spin_unlock(&data->lock, key);
	return 0;
}

static int agm_timer_get_value(const struct device *dev, uint32_t *ticks)
{
	const struct agm_timer_cfg *cfg = dev->config;
	struct agm_timer_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	*ticks = agm_timer_now(cfg, data);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int agm_timer_set_alarm(const struct device *dev, uint8_t chan_id,
			       const struct counter_alarm_cfg *alarm_cfg)
{
	const struct agm_timer_cfg *cfg = dev->config;
	struct agm_timer_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t now, target, delta;

	if (chan_id >= AGM_TIMER_NUM_CH || alarm_cfg == NULL ||
	    alarm_cfg->callback == NULL) {
		/* See counter_agm_gptimer.c: AGM_TIMER_TOP is UINT32_MAX, so the
		 * old `ticks > AGM_TIMER_TOP` test was always false. */
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);

	if (!data->running) {
		k_spin_unlock(&data->lock, key);
		return -EIO;
	}
	if (data->ch[chan_id].callback != NULL) {
		k_spin_unlock(&data->lock, key);
		return -EBUSY;
	}

	now = agm_timer_now(cfg, data);
	if (alarm_cfg->flags & COUNTER_ALARM_CFG_ABSOLUTE) {
		target = alarm_cfg->ticks;
	} else {
		target = now + alarm_cfg->ticks;
	}
	delta = target - now; /* wraps mod 2^32 */
	if (delta == 0U) {
		/* Alarm already due: expire on the very next tick. */
		delta = 1U;
		target = now + 1U;
	}

	data->ch[chan_id].callback = alarm_cfg->callback;
	data->ch[chan_id].user_data = alarm_cfg->user_data;
	data->ch[chan_id].target = target;

	/* Count down exactly `delta` ticks with the interrupt armed. */
	agm_timer_sub_load(cfg, chan_id, delta, true);

	k_spin_unlock(&data->lock, key);
	return 0;
}

static int agm_timer_cancel_alarm(const struct device *dev, uint8_t chan_id)
{
	const struct agm_timer_cfg *cfg = dev->config;
	struct agm_timer_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t now;

	if (chan_id >= AGM_TIMER_NUM_CH) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);

	if (data->ch[chan_id].callback == NULL) {
		k_spin_unlock(&data->lock, key);
		return 0;
	}

	if (chan_id == 0U) {
		/* Keep the reported value continuous while restoring
		 * sub-timer 1 to its free-running top: read the value
		 * first (armed formula), then clear the alarm.
		 */
		now = data->ch[0].target - agm_timer_read(cfg, 0U,
							  AGM_TIMER_VALUE);
		data->ch[0].callback = NULL;
		data->ch[0].target = 0U;
		data->phase = now;
		agm_timer_sub_load(cfg, 0U, AGM_TIMER_TOP,
				   data->top_callback != NULL);
	} else {
		data->ch[1].callback = NULL;
		data->ch[1].target = 0U;
		agm_timer_sub_reset(cfg, 1U);
	}

	k_spin_unlock(&data->lock, key);
	return 0;
}

static int agm_timer_set_top_value(const struct device *dev,
				   const struct counter_top_cfg *cfg)
{
	struct agm_timer_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t ctrl;

	if (cfg == NULL) {
		return -EINVAL;
	}
	if (cfg->ticks != AGM_TIMER_TOP) {
		/* The 32-bit free-run top is fixed by the driver. */
		return -ENOTSUP;
	}

	key = k_spin_lock(&data->lock);

	if (data->ch[0].callback != NULL || data->ch[1].callback != NULL) {
		k_spin_unlock(&data->lock, key);
		return -EBUSY;
	}

	data->top_callback = cfg->callback;
	data->top_user_data = cfg->user_data;

	if (data->running) {
		ctrl = AGM_TIMER_CTRL_FREE_RUN | AGM_TIMER_CTRL_ENABLE;
		if (cfg->callback != NULL) {
			agm_timer_write(dev->config, 0U, AGM_TIMER_INTCLR,
					AGM_TIMER_INT_MSK);
			ctrl |= AGM_TIMER_CTRL_INT_EN;
		}
		agm_timer_write(dev->config, 0U, AGM_TIMER_CTRL, ctrl);
	}

	k_spin_unlock(&data->lock, key);
	return 0;
}

static uint32_t agm_timer_get_pending_int(const struct device *dev)
{
	const struct agm_timer_cfg *cfg = dev->config;

	return agm_timer_read(cfg, 0U, AGM_TIMER_MIS) |
	       agm_timer_read(cfg, 1U, AGM_TIMER_MIS);
}

static uint32_t agm_timer_get_top_value(const struct device *dev)
{
	return AGM_TIMER_TOP;
}

static uint32_t agm_timer_get_freq(const struct device *dev)
{
	const struct agm_timer_cfg *cfg = dev->config;

	return cfg->pclk_hz;
}

/* Zephyr's ISR type -- see counter_agm_gptimer.c / uart_agm.c for why. */
static void agm_timer_isr(const void *isr_arg)
{
	const struct device *dev = isr_arg;
	const struct agm_timer_cfg *cfg = dev->config;
	struct agm_timer_data *data = dev->data;
	counter_alarm_callback_t alarm_cb = NULL;
	counter_top_callback_t top_cb = NULL;
	void *alarm_user = NULL;
	void *top_user = NULL;
	uint32_t ticks = 0U;
	uint8_t chan_id = 0U;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);

	for (uint8_t sub = 0U; sub < AGM_TIMER_NUM_CH; sub++) {
		if ((agm_timer_read(cfg, sub, AGM_TIMER_MIS) &
		     AGM_TIMER_INT_MSK) == 0U) {
			continue;
		}
		agm_timer_write(cfg, sub, AGM_TIMER_INTCLR, AGM_TIMER_INT_MSK);

		if (sub == 0U) {
			if (data->ch[0].callback != NULL) {
				/* Channel-0 alarm matched: report it and
				 * restore sub-timer 1 to free-run.
				 */
				alarm_cb = data->ch[0].callback;
				alarm_user = data->ch[0].user_data;
				ticks = data->ch[0].target;
				chan_id = 0U;
				data->ch[0].callback = NULL;
				data->ch[0].target = 0U;

				data->phase = ticks;
				agm_timer_sub_load(cfg, 0U, AGM_TIMER_TOP,
						   data->top_callback != NULL);
			} else if (data->top_callback != NULL) {
				/* Free-run counter wrapped past the top. */
				top_cb = data->top_callback;
				top_user = data->top_user_data;
			}
		} else {
			/* Channel-1 alarm matched (sub-timer 2). */
			alarm_cb = data->ch[1].callback;
			alarm_user = data->ch[1].user_data;
			ticks = data->ch[1].target;
			chan_id = 1U;
			data->ch[1].callback = NULL;
			data->ch[1].target = 0U;
			agm_timer_sub_reset(cfg, 1U);
		}
	}

	k_spin_unlock(&data->lock, key);

	if (alarm_cb != NULL) {
		alarm_cb(dev, chan_id, ticks, alarm_user);
	}
	if (top_cb != NULL) {
		top_cb(dev, top_user);
	}
}

static int agm_timer_init(const struct device *dev)
{
	const struct agm_timer_cfg *cfg = dev->config;

	/* The unit's APB gate (agm,apb-clkenable-bit = APB_MASK_TIMER0/1) was
	 * opened by soc.c at PRE_KERNEL_1 from devicetree. That also removes
	 * the old address->bit mapping and its "reject anything outside the
	 * two dual-timer pages" guard. */

	/* Both sub-timers disabled and loaded with the top value. */
	agm_timer_sub_load(cfg, 0U, AGM_TIMER_TOP, false);
	agm_timer_sub_reset(cfg, 1U);

	cfg->irq_config();
	LOG_INF("AGM dual timer initialized (pclk %u Hz)", cfg->pclk_hz);
	return 0;
}

static DEVICE_API(counter, agm_timer_api) = {
	.start = agm_timer_start,
	.stop = agm_timer_stop,
	.get_value = agm_timer_get_value,
	.set_alarm = agm_timer_set_alarm,
	.cancel_alarm = agm_timer_cancel_alarm,
	.set_top_value = agm_timer_set_top_value,
	.get_pending_int = agm_timer_get_pending_int,
	.get_top_value = agm_timer_get_top_value,
	.get_freq = agm_timer_get_freq,
};

#define AGM_TIMER_IRQ_CONNECT(n)						\
	static void agm_timer_irq_config_##n(void)				\
	{									\
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority),		\
			    agm_timer_isr, DEVICE_DT_INST_GET(n), 0);		\
		irq_enable(DT_INST_IRQN(n));					\
	}

/* The APB gate bit is derived from the reg address, so it must be one of
 * the two dual-timer pages.
 */
#define AGM_TIMER_INIT(n)							\
	BUILD_ASSERT(DT_INST_REG_ADDR(n) == AGM_TIMER_BASE(0) ||	\
		     DT_INST_REG_ADDR(n) == AGM_TIMER_BASE(1),		\
		     "timer" #n " reg address is not 0x4001E000/0x4001F000"); \
	AGM_TIMER_IRQ_CONNECT(n)						\
	static const struct agm_timer_cfg agm_timer_cfg_##n = {			\
		.info = {							\
			.max_top_value = AGM_TIMER_TOP,				\
			.freq = DT_INST_PROP_BY_PHANDLE(n, clocks, clock_frequency),		\
			.flags = COUNTER_CONFIG_INFO_COUNT_UP,			\
			.channels = AGM_TIMER_NUM_CH,				\
		},								\
		.base = DT_INST_REG_ADDR(n),					\
		.pclk_hz = DT_INST_PROP_BY_PHANDLE(n, clocks, clock_frequency),			\
		.irq_config = agm_timer_irq_config_##n,				\
	};									\
	static struct agm_timer_data agm_timer_data_##n;			\
	DEVICE_DT_INST_DEFINE(n, agm_timer_init, NULL, &agm_timer_data_##n,	\
			      &agm_timer_cfg_##n, POST_KERNEL,			\
			      CONFIG_COUNTER_INIT_PRIORITY, &agm_timer_api);

DT_INST_FOREACH_STATUS_OKAY(AGM_TIMER_INIT)
