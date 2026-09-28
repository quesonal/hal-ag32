/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AGM AgRV2K DMA controller driver.
 *
 * The controller is an ARM PL080 / LPC23xx-GPDMA-style DMAC with 8
 * channels: global registers at 0x00-0x3C, per-channel SrcAddr /
 * DstAddr / LLI / Control / Configuration at 0x100 + ch*0x20.
 *
 * Supported transfer model (v1 DMA API):
 *   - one-shot transfers only (no cyclic / no reload)
 *   - source and destination data widths must match (8/16/32-bit)
 *   - blocks are flattened into a linked-list (LLI) chain in SRAM;
 *     each LLI carries up to 4092 source-width units, matching the
 *     AgRV SDK's DMAC_MAX_TRANSFER_SIZE
 *   - terminal-count and error PLIC interrupts drive completion
 *     callbacks. All three IRQ lines are required by the binding and
 *     hard-wired here (AGM_DMA_IRQ_CONFIG below); dma_get_status()
 *     additionally reports a completion the caller observed by
 *     polling ENBLDCHNS, but the driver has no IRQ-less mode.
 *
 * SoC glue done here at init: AHB clock enable + reset deassert
 * (SYS controller, mirror of the SDK's board_init).
 */

#define DT_DRV_COMPAT agm_agrv2k_dma

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <agm_sys.h>

LOG_MODULE_REGISTER(dma_agm, CONFIG_DMA_LOG_LEVEL);

/* Global DMAC registers. */
#define AGM_DMA_INTTCSTATUS	0x004U
#define AGM_DMA_INTTCCLEAR	0x008U
#define AGM_DMA_INTERRSTATUS	0x00cU
#define AGM_DMA_INTERRCLEAR	0x010U
#define AGM_DMA_ENBLDCHNS	0x01cU
#define AGM_DMA_CONFIGURATION	0x030U
/* Synchronization register: a bit set to 1 *disables* the request
 * synchronization logic for that request line. The
 * register resets to 0, i.e. every line synchronized. */
#define AGM_DMA_SYNC		0x034U

#define AGM_DMA_CONFIG_E	BIT(0)

/* Per-channel register stride. */
#define AGM_DMA_CHAN_BASE	0x100U
#define AGM_DMA_CHAN_STRIDE	0x20U
#define AGM_DMA_CHAN_SRCADDR	0x00U
#define AGM_DMA_CHAN_DSTADDR	0x04U
#define AGM_DMA_CHAN_LLI	0x08U
#define AGM_DMA_CHAN_CONTROL	0x0cU
#define AGM_DMA_CHAN_CONFIG	0x10U

/* Channel control field layout (LPC23xx GPDMA / AgRV SDK). */
#define AGM_DMA_CTRL_SIZE_OFF	0U
#define AGM_DMA_CTRL_SIZE_MASK	0xfffU
#define AGM_DMA_CTRL_SBSIZE_OFF	12U
#define AGM_DMA_CTRL_DBSIZE_OFF	15U
#define AGM_DMA_CTRL_SWIDTH_OFF	18U
#define AGM_DMA_CTRL_DWIDTH_OFF	21U
#define AGM_DMA_CTRL_SRC_MASTER	BIT(24)
#define AGM_DMA_CTRL_DST_MASTER	BIT(25)
#define AGM_DMA_CTRL_SRC_INCR	BIT(26)
#define AGM_DMA_CTRL_DST_INCR	BIT(27)
#define AGM_DMA_CTRL_TC_INT	BIT(31)

/* Channel configuration field layout (AgRV SDK). */
#define AGM_DMA_CFG_E			BIT(0)
#define AGM_DMA_CFG_SRC_PERIPH_OFF	1U
#define AGM_DMA_CFG_DST_PERIPH_OFF	6U
#define AGM_DMA_CFG_FLOW_OFF		11U
#define AGM_DMA_CFG_ERR_INT		BIT(14)
#define AGM_DMA_CFG_TC_INT		BIT(15)
#define AGM_DMA_CFG_ACTIVE		BIT(17)
#define AGM_DMA_CFG_HALT		BIT(18)

/* Flow control types (PL080 encoding; the value says *who* controls the
 * transfer).
 *
 * With the SPI engine on AgRV2K (SPI1 @ 100 MHz, SPI TX DMA into
 * PHASE_DATA[1], 8 words, verified through the flash's write-enable latch):
 *
 *   M2P flow 1 (DMA controls), size 0 or 8, either master bit -> TIMEOUT
 *   M2P flow 5 (peripheral controls)                        -> DONE
 *
 * so a memory-to-peripheral transfer *must* let the peripheral control it.
 * P2M stays at 2: the SPI RX DMA path (bounce buffer, page reads) was
 * verified with it, and the peripheral-controlled 6 did not complete when it
 * was tried. M2M is unaffected.
 *
 * Do not change these numbers without re-running a peripheral transfer on
 * hardware -- and note that a failed SPI DMA transfer leaves the engine
 * mid-communication, which poisons whatever runs next (spi_agm resets the
 * engine on a DMA timeout for that reason). */
#define AGM_DMA_FLOW_M2M	0U
#define AGM_DMA_FLOW_M2P	5U /* DMAC_MEM_TO_PERIPHERAL_PERIPHERAL_CTRL */
#define AGM_DMA_FLOW_P2M	2U
#define AGM_DMA_FLOW_P2P	3U

#define AGM_DMA_MAX_CHANNELS	8U
#define AGM_DMA_MAX_LLI		16U
#define AGM_DMA_MAX_XFER_UNITS	4092U /* 12-bit size field, multiple of 4 */

struct agm_dma_lli {
	uint32_t src;
	uint32_t dst;
	uint32_t lli;
	uint32_t control;
};

struct agm_dma_chan {
	struct dma_config cfg;
	bool busy;
	struct agm_dma_lli lli[AGM_DMA_MAX_LLI];
};

struct agm_dma_data {
	struct agm_dma_chan chan[AGM_DMA_MAX_CHANNELS];
};

struct agm_dma_cfg {
	uint32_t base;
	uint8_t num_channels;
	void (*irq_config)(void);
};

static inline uint32_t agm_dma_read(const struct agm_dma_cfg *cfg, uint32_t off)
{
	return sys_read32(cfg->base + off);
}

static inline void agm_dma_write(const struct agm_dma_cfg *cfg, uint32_t off,
				 uint32_t val)
{
	sys_write32(val, cfg->base + off);
}

static inline uint32_t agm_dma_chan_off(const struct agm_dma_cfg *cfg,
					uint32_t ch, uint32_t reg)
{
	(void)cfg;
	return AGM_DMA_CHAN_BASE + ch * AGM_DMA_CHAN_STRIDE + reg;
}

static struct agm_dma_chan *agm_dma_get_chan(const struct device *dev,
					     uint32_t ch)
{
	struct agm_dma_data *data = dev->data;

	return &data->chan[ch];
}

/* Every entry point that takes a channel number from the caller has to pass
 * through this first: agm_dma_get_chan() indexes data->chan[] with no bound. */
static bool agm_dma_channel_ok(const struct agm_dma_cfg *cfg, uint32_t ch)
{
	return ch < (uint32_t)cfg->num_channels;
}

/* Encode a per-burst transfer count into the 3-bit HW encoding.
 * The HW supports 1/4/8/.../256 transfers per burst (AgRV SDK enum).
 */
static uint32_t agm_dma_burst_code(uint32_t transfers)
{
	uint32_t code = 0;

	if (transfers >= 256U) {
		code = 7U;
	} else if (transfers >= 128U) {
		code = 6U;
	} else if (transfers >= 64U) {
		code = 5U;
	} else if (transfers >= 32U) {
		code = 4U;
	} else if (transfers >= 16U) {
		code = 3U;
	} else if (transfers >= 8U) {
		code = 2U;
	} else if (transfers >= 4U) {
		code = 1U;
	} else {
		code = 0U;
	}
	return code;
}

static int agm_dma_config(const struct device *dev, uint32_t channel,
			  struct dma_config *config)
{
	const struct agm_dma_cfg *cfg = dev->config;
	struct agm_dma_chan *chan;

	if (!agm_dma_channel_ok(cfg, channel)) {
		LOG_ERR("channel %u out of range (max %u)", channel,
			cfg->num_channels);
		return -EINVAL;
	}
	if (config == NULL || config->head_block == NULL ||
	    config->block_count == 0U) {
		LOG_ERR("no transfer block configured");
		return -EINVAL;
	}
	if (config->source_data_size != config->dest_data_size) {
		LOG_ERR("mixed source/dest widths not supported");
		return -EINVAL;
	}
	if (config->source_data_size != 1U && config->source_data_size != 2U &&
	    config->source_data_size != 4U) {
		LOG_ERR("unsupported data width %u",
			config->source_data_size);
		return -EINVAL;
	}
	switch (config->channel_direction) {
	case MEMORY_TO_MEMORY:
	case MEMORY_TO_PERIPHERAL:
	case PERIPHERAL_TO_MEMORY:
	case PERIPHERAL_TO_PERIPHERAL:
		break;
	default:
		LOG_ERR("unsupported direction %u", config->channel_direction);
		return -EINVAL;
	}

	chan = agm_dma_get_chan(dev, channel);
	if (chan->busy) {
		return -EBUSY;
	}

	memcpy(&chan->cfg, config, sizeof(*config));
	return 0;
}

/* Flatten the dma_config block list into the channel's LLI array and
 * program the channel registers. Returns 0 on success. */
static int agm_dma_program(const struct device *dev, uint32_t channel)
{
	const struct agm_dma_cfg *cfg = dev->config;
	struct agm_dma_chan *chan = agm_dma_get_chan(dev, channel);
	struct dma_block_config *block = chan->cfg.head_block;
	uint32_t width = chan->cfg.source_data_size;
	uint32_t width_code = (width == 1U) ? 0U : (width == 2U) ? 1U : 2U;
	uint32_t burst = agm_dma_burst_code(chan->cfg.source_burst_length / width);
	uint32_t nlli = 0U;
	bool src_incr, dst_incr;

	if (width == 0U) {
		return -EINVAL;
	}

	while (block != NULL) {
		uint32_t remaining = block->block_size;
		uint32_t src = block->source_address;
		uint32_t dst = block->dest_address;

		if (block->source_addr_adj == DMA_ADDR_ADJ_NO_CHANGE) {
			src_incr = false;
		} else if (block->source_addr_adj == DMA_ADDR_ADJ_INCREMENT) {
			src_incr = true;
		} else {
			LOG_ERR("source address decrement not supported");
			return -EINVAL;
		}
		if (block->dest_addr_adj == DMA_ADDR_ADJ_NO_CHANGE) {
			dst_incr = false;
		} else if (block->dest_addr_adj == DMA_ADDR_ADJ_INCREMENT) {
			dst_incr = true;
		} else {
			LOG_ERR("dest address decrement not supported");
			return -EINVAL;
		}
		if ((remaining % width) != 0U) {
			LOG_ERR("block size %u not multiple of width %u",
				remaining, width);
			return -EINVAL;
		}

		while (remaining > 0U) {
			uint32_t units = MIN(remaining / width,
					     AGM_DMA_MAX_XFER_UNITS);
			uint32_t bytes = units * width;
			struct agm_dma_lli *lli;

			if (nlli >= AGM_DMA_MAX_LLI) {
				LOG_ERR("transfer too large for LLI pool");
				return -EINVAL;
			}
			lli = &chan->lli[nlli];
			lli->src = src;
			lli->dst = dst;
			lli->lli = 0U; /* relinked after the loop */
			lli->control = ((units & AGM_DMA_CTRL_SIZE_MASK)
					<< AGM_DMA_CTRL_SIZE_OFF) |
				       (burst << AGM_DMA_CTRL_SBSIZE_OFF) |
				       (burst << AGM_DMA_CTRL_DBSIZE_OFF) |
				       (width_code << AGM_DMA_CTRL_SWIDTH_OFF) |
				       (width_code << AGM_DMA_CTRL_DWIDTH_OFF) |
				       AGM_DMA_CTRL_SRC_MASTER |
				       AGM_DMA_CTRL_DST_MASTER |
				       (src_incr ? AGM_DMA_CTRL_SRC_INCR : 0U) |
				       (dst_incr ? AGM_DMA_CTRL_DST_INCR : 0U);
			nlli++;
			remaining -= bytes;
			if (src_incr) {
				src += bytes;
			}
			if (dst_incr) {
				dst += bytes;
			}
		}
		block = block->next_block;
	}

	/* Every block asked for zero bytes: there is no LLI to link, and
	 * chan->lli[0] still holds the *previous* transfer's src/dst/control.
	 * Returning 0 here would let agm_dma_start() set busy and enable the
	 * channel on that stale LLI, i.e. replay an old transfer. */
	if (nlli == 0U) {
		LOG_ERR("ch%u: zero-length transfer (nothing to program)", channel);
		return -EINVAL;
	}

	/* Link the LLI chain; the terminal-count interrupt is enabled
	 * only on the final item. */
	for (uint32_t i = 0; i < nlli; i++) {
		if (i + 1U < nlli) {
			chan->lli[i].lli =
				(uint32_t)(uintptr_t)&chan->lli[i + 1U];
		} else {
			chan->lli[i].lli = 0U;
			chan->lli[i].control |= AGM_DMA_CTRL_TC_INT;
		}
	}

	/* Program the channel from the first LLI. */
	agm_dma_write(cfg, agm_dma_chan_off(cfg, channel, AGM_DMA_CHAN_SRCADDR),
		      chan->lli[0].src);
	agm_dma_write(cfg, agm_dma_chan_off(cfg, channel, AGM_DMA_CHAN_DSTADDR),
		      chan->lli[0].dst);
	agm_dma_write(cfg, agm_dma_chan_off(cfg, channel, AGM_DMA_CHAN_LLI),
		      chan->lli[0].lli);
	agm_dma_write(cfg, agm_dma_chan_off(cfg, channel, AGM_DMA_CHAN_CONTROL),
		      chan->lli[0].control);

	LOG_DBG("ch%u: programmed %u LLI items", channel, nlli);
	return 0;
}

static int agm_dma_start(const struct device *dev, uint32_t channel)
{
	const struct agm_dma_cfg *cfg = dev->config;
	struct agm_dma_chan *chan;
	uint32_t src_periph = 0U;
	uint32_t dst_periph = 0U;
	uint32_t flow = AGM_DMA_FLOW_M2M;
	uint32_t ch_cfg;
	int ret;

	if (!agm_dma_channel_ok(cfg, channel)) {
		return -EINVAL;
	}

	chan = agm_dma_get_chan(dev, channel);
	if (chan->busy) {
		return -EBUSY;
	}

	switch (chan->cfg.channel_direction) {
	case MEMORY_TO_MEMORY:
		flow = AGM_DMA_FLOW_M2M;
		break;
	case MEMORY_TO_PERIPHERAL:
		flow = AGM_DMA_FLOW_M2P;
		dst_periph = chan->cfg.dma_slot & 0xfU;
		break;
	case PERIPHERAL_TO_MEMORY:
		flow = AGM_DMA_FLOW_P2M;
		src_periph = chan->cfg.dma_slot & 0xfU;
		break;
	case PERIPHERAL_TO_PERIPHERAL:
		flow = AGM_DMA_FLOW_P2P;
		src_periph = chan->cfg.dma_slot & 0xfU;
		dst_periph = chan->cfg.dma_slot & 0xfU;
		break;
	default:
		return -EINVAL;
	}

	/* Clear stale status bits. */
	agm_dma_write(cfg, AGM_DMA_INTTCCLEAR, BIT(channel));
	agm_dma_write(cfg, AGM_DMA_INTERRCLEAR, BIT(channel));

	/* The SPI engine (and the other peripherals this driver serves) run on
	 * the same clock as the DMAC, and their request lines are single-cycle
	 * pulses (DMA_TX_SREQ/DMA_RX_SREQ in the SPI chapter). With the
	 * synchronization logic enabled those pulses can be missed, so disable
	 * it for the request line the client asked for -- the same thing the
	 * SDK's DMAC_DisableSyncRequest() does: the SPI RX DMA worked either
	 * way, the TX DMA only completes with this. */
	if ((flow == AGM_DMA_FLOW_M2P) || (flow == AGM_DMA_FLOW_P2M) ||
	    (flow == AGM_DMA_FLOW_P2P)) {
		uint32_t request = chan->cfg.dma_slot & 0xfU;

		if (request != 0U) {
			agm_dma_write(cfg, AGM_DMA_SYNC,
				      agm_dma_read(cfg, AGM_DMA_SYNC) | BIT(request));
		}
	}

	ret = agm_dma_program(dev, channel);
	if (ret != 0) {
		return ret;
	}

	ch_cfg = AGM_DMA_CFG_E | AGM_DMA_CFG_ERR_INT | AGM_DMA_CFG_TC_INT |
		 (src_periph << AGM_DMA_CFG_SRC_PERIPH_OFF) |
		 (dst_periph << AGM_DMA_CFG_DST_PERIPH_OFF) |
		 (flow << AGM_DMA_CFG_FLOW_OFF);
	chan->busy = true;
	agm_dma_write(cfg, agm_dma_chan_off(cfg, channel, AGM_DMA_CHAN_CONFIG),
		      ch_cfg);
	return 0;
}

static int agm_dma_stop(const struct device *dev, uint32_t channel)
{
	const struct agm_dma_cfg *cfg = dev->config;
	struct agm_dma_chan *chan;
	uint32_t reg_off;
	uint32_t ch_cfg;
	uint32_t tries = 1000000U;

	if (!agm_dma_channel_ok(cfg, channel)) {
		return -EINVAL;
	}

	chan = agm_dma_get_chan(dev, channel);
	reg_off = agm_dma_chan_off(cfg, channel, AGM_DMA_CHAN_CONFIG);
	ch_cfg = agm_dma_read(cfg, reg_off);

	if ((ch_cfg & AGM_DMA_CFG_ACTIVE) != 0U) {
		/* Halt first so no new requests are taken, then wait for
		 * the active bit to clear. */
		agm_dma_write(cfg, reg_off, ch_cfg | AGM_DMA_CFG_HALT);
		while (tries-- > 0U) {
			if ((agm_dma_read(cfg, reg_off) &
			     AGM_DMA_CFG_ACTIVE) == 0U) {
				break;
			}
		}
	}
	agm_dma_write(cfg, reg_off, 0U);
	agm_dma_write(cfg, AGM_DMA_INTTCCLEAR, BIT(channel));
	agm_dma_write(cfg, AGM_DMA_INTERRCLEAR, BIT(channel));
	chan->busy = false;
	return 0;
}

static int agm_dma_get_status(const struct device *dev, uint32_t channel,
			      struct dma_status *status)
{
	const struct agm_dma_cfg *cfg = dev->config;
	struct agm_dma_chan *chan;
	uint32_t en;

	if (status == NULL || !agm_dma_channel_ok(cfg, channel)) {
		return -EINVAL;
	}

	chan = agm_dma_get_chan(dev, channel);
	en = agm_dma_read(cfg, AGM_DMA_ENBLDCHNS);
	memset(status, 0, sizeof(*status));
	status->dir = (enum dma_channel_direction)chan->cfg.channel_direction;

	if ((en & BIT(channel)) != 0U) {
		status->busy = true;
	} else {
		/* Transfer finished (channel self-disables): report the
		 * completion once to a caller that polled instead of waiting for
		 * the TC interrupt. */
		bool was_busy = chan->busy;
		dma_callback_t cb = chan->cfg.dma_callback;

		chan->busy = false;
		status->busy = false;
		if (was_busy && cb != NULL) {
			cb(dev, chan->cfg.user_data, channel,
			   DMA_STATUS_COMPLETE);
		}
	}
	return 0;
}

/* Zephyr's ISR type, like the UART driver's (see uart_agm.c for the full
 * reason): IRQ_CONNECT() pastes __isr_ ## <handler> to build the table symbol,
 * so the handler has to be a bare identifier and cannot be cast at the call
 * site -- and the native/POSIX arch's ARCH_IRQ_CONNECT() is the only one that
 * does not cast it, which makes a `const struct device *` handler
 * -Wincompatible-pointer-types there. */
static void agm_dma_isr(const void *isr_arg)
{
	const struct device *dev = isr_arg;
	const struct agm_dma_cfg *cfg = dev->config;
	struct agm_dma_data *data = dev->data;
	uint32_t tc = agm_dma_read(cfg, AGM_DMA_INTTCSTATUS);
	uint32_t err = agm_dma_read(cfg, AGM_DMA_INTERRSTATUS);
	uint32_t ch;

	for (ch = 0; ch < cfg->num_channels; ch++) {
		struct agm_dma_chan *chan = &data->chan[ch];

		if ((tc & BIT(ch)) != 0U) {
			agm_dma_write(cfg, AGM_DMA_INTTCCLEAR, BIT(ch));
			chan->busy = false;
			if (chan->cfg.dma_callback != NULL) {
				chan->cfg.dma_callback(dev, chan->cfg.user_data,
						       ch, DMA_STATUS_COMPLETE);
			}
		}
		if ((err & BIT(ch)) != 0U) {
			agm_dma_write(cfg, AGM_DMA_INTERRCLEAR, BIT(ch));
			chan->busy = false;
			if (chan->cfg.dma_callback != NULL) {
				chan->cfg.dma_callback(dev, chan->cfg.user_data,
						       ch, -EIO);
			}
		}
	}
}

#define AGM_DMA_IRQ_CONNECT(n, idx)					     \
	IRQ_CONNECT(DT_INST_IRQN_BY_IDX(n, idx),			     \
		    DT_INST_IRQ_BY_IDX(n, idx, priority),		     \
		    agm_dma_isr, DEVICE_DT_INST_GET(n),		     \
		    0)

#define AGM_DMA_IRQ_CONFIG(n)						     \
	static void agm_dma_irq_config_##n(void)			     \
	{								     \
		AGM_DMA_IRQ_CONNECT(n, 0);				     \
		AGM_DMA_IRQ_CONNECT(n, 1);				     \
		AGM_DMA_IRQ_CONNECT(n, 2);				     \
		irq_enable(DT_INST_IRQN_BY_IDX(n, 0));			     \
		irq_enable(DT_INST_IRQN_BY_IDX(n, 1));			     \
		irq_enable(DT_INST_IRQN_BY_IDX(n, 2));			     \
	}

static int agm_dma_init(const struct device *dev)
{
	const struct agm_dma_cfg *cfg = dev->config;

	/* DMAC0's AHB clock and reset release (agm,ahb-clkenable-bit /
	 * agm,ahb-reset-bit) were applied by soc.c at PRE_KERNEL_1 from
	 * devicetree, together with USB0's. */

	/* Controller-level init (mirrors SDK DMAC_Init): disable all
	 * channels, clear status, then enable the DMAC. */
	agm_dma_write(cfg, AGM_DMA_CONFIGURATION, 0U);
	for (uint32_t ch = 0; ch < cfg->num_channels; ch++) {
		agm_dma_write(cfg,
			      agm_dma_chan_off(cfg, ch, AGM_DMA_CHAN_CONFIG),
			      0U);
	}
	agm_dma_write(cfg, AGM_DMA_INTERRCLEAR, 0xffffffffU);
	agm_dma_write(cfg, AGM_DMA_INTTCCLEAR, 0xffffffffU);
	agm_dma_write(cfg, AGM_DMA_CONFIGURATION, AGM_DMA_CONFIG_E);

	/* Never NULL: AGM_DMA_INIT() assigns the per-instance hook
	 * unconditionally (see the BUILD_ASSERT on DT_INST_NUM_IRQS there). */
	cfg->irq_config();
	LOG_INF("AGM DMAC initialized (%u channels)", cfg->num_channels);
	return 0;
}

static DEVICE_API(dma, agm_dma_api) = {
	.config = agm_dma_config,
	.start = agm_dma_start,
	.stop = agm_dma_stop,
	.get_status = agm_dma_get_status,
};

#define AGM_DMA_INIT(n)							\
	AGM_DMA_IRQ_CONFIG(n)						\
	/* All three lines (intr / inttc / interr) are hard-wired by	\
	 * AGM_DMA_IRQ_CONFIG(): DT_INST_IRQN_BY_IDX(n, 1|2) below is a	\
	 * compile-time lookup, so a node with fewer interrupts is a build	\
	 * error, not a "no IRQ" configuration. State that here, and in the	\
	 * binding, instead of pretending the driver can fall back to	\
	 * polling. */							\
	BUILD_ASSERT(DT_INST_NUM_IRQS(n) == 3,				\
		     "agm,agrv2k-dma needs all three interrupts (intr, inttc, interr)"); \
	static const struct agm_dma_cfg agm_dma_cfg_##n = {		\
		.base = DT_INST_REG_ADDR(n),				\
		.num_channels = DT_INST_PROP(n, dma_channels),		\
		.irq_config = agm_dma_irq_config_##n,			\
	};								\
	static struct agm_dma_data agm_dma_data_##n;			\
	DEVICE_DT_INST_DEFINE(n, agm_dma_init, NULL,			\
			      &agm_dma_data_##n,			\
			      &agm_dma_cfg_##n, POST_KERNEL,		\
			      CONFIG_DMA_INIT_PRIORITY,			\
			      &agm_dma_api);

DT_INST_FOREACH_STATUS_OKAY(AGM_DMA_INIT)
