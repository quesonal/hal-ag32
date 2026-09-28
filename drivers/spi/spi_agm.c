/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief AgRV2K SPI controller driver (phase engine).
 *
 * The IP (SPI0 @ 0x40012000, SPI1 @ 0x40013000) is a phase engine: one
 * START runs up to eight phases back to back inside a single chip-select
 * window. Each phase has an action (TX / DUMMY_TX / RX / POLL), a line
 * mode (single / dual / quad) and a byte count; the data of a phase lives
 * in that phase's 32-bit PHASE_DATA register.
 *
 * The vendor's own summary of what that means for a caller:
 *
 *   1. the first phase must be TX -- a transfer cannot start with RX;
 *   2. RX and TX never overlap (TX phases, then RX phases);
 *   3. RX must be the last phase.
 *
 * This driver therefore implements the half-duplex, TX-then-RX shape that
 * SPI flash and similar command/response devices use: the request's TX
 * buffers are programmed as TX phases and its RX buffers as RX phases.
 * Requests the engine cannot express are rejected with -ENOTSUP instead
 * of being silently mis-clocked:
 *
 *   * RX with no TX (nothing to clock the first phase with),
 *   * CPOL/CPHA other than mode 0 (the mode bits live in the fabric, see
 *     the vendor's full_duplex_spi.v patch, not in this controller),
 *   * data words other than 8 bits, dual/quad line modes,
 *   * more than 4 bytes of RX: the engine streams a phase longer than 4
 *     bytes through that phase's data register with DMA (the SDK's
 *     SPI_SendAndRecvDMA), so without DMA only 4 bytes come back. The
 *     vendor's own summary is "TX up to 4 bytes, RX unlimited" -- the
 *     "unlimited" RX relies on the DMA path.
 *
 * RX counts slots the way the SPI API does, which matters because this
 * engine is half duplex: byte i of the frame is clocked with tx[i] and
 * lands in rx[i], but MISO is only sampled once the TX phases are done. A
 * caller that follows the usual full-duplex idiom -- "send a command, then
 * read the answer" with the command's slots also present in its RX list
 * (upstream spi-nor does exactly that: rx[0..3] are the command echo and
 * the data starts at rx[4]) -- gets those leading slots back as 0xFF, and
 * the bytes the engine did capture in their proper places. Asking for RX
 * bytes that *all* fall inside the TX window is -ENOTSUP: there is no such
 * phase to run, and silently returning filler would look like real data.
 *
 * TX is different: several TX phases each carry their own 4 bytes, which is
 * exactly how the SDK sends a long command without DMA (see
 * SPI_SendAndReceiveDMAStart: it fills PHASE_DATA[0..n] in a loop). Up to
 * eight phases are available, so this driver sends up to 32 bytes that way;
 * beyond that, and for RX longer than four bytes, it switches to DMA into
 * (or out of) the phase data register -- PHASE_DATA is the DMA port, with
 * SPIx_TX_DMA_REQ / SPIx_RX_DMA_REQ as the handshake lines. Those paths need
 * the DT `dmas` specifiers, 4-byte-aligned lengths (the controller moves
 * 32-bit words), and a single contiguous TX buffer.
 *
 * Do not build several RX phases instead:
 * The controller only refreshes the data register of the
 * *last* phase, so a "TX + RX + RX" transfer returns stale bytes for the
 * earlier RX phase. One TX phase plus one RX phase is the whole direct
 * (non-DMA) contract, which is exactly what the vendor documents.
 *
 * Phases with more than 4 bytes, and therefore bulk flash reads, use the
 * DMA path the SDK uses (PHASE_DATA as the DMA port, SPI_TX/RX_DMA_REQ as
 * the handshake).
 */

#define DT_DRV_COMPAT agm_agrv2k_spi

/* Defined below; the extension entry points check it to reject other devices.
 *
 * DEVICE_API(), like every other driver in this tree and every upstream SPI
 * driver: it places the table in the class's iterable API section, which is
 * what DEVICE_API_GET() checks (CONFIG_DEVICE_API_ASSERT, default y under
 * CONFIG_ASSERT). With a plain `static const struct spi_driver_api` the check
 * fails and every spi_transceive()/spi_write() call panics with "device API is
 * not spi" -- i.e. the driver was unusable in any CONFIG_ASSERT build, which is
 * every ztest/native config (found while adding this suite: the panic is the
 * first thing that happens). The forward declaration keeps the extension entry
 * points' identity check working. */
extern const struct spi_driver_api spi_agm_api;

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/spi/spi_agm.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <agm_sys.h>

/* Register map (AgRV SDK framework-agrv_sdk/src/spi.h). */
#define SPI_AGM_CTRL              0x00U
#define SPI_AGM_PHASE_CTRL(n)     (0x10U + 4U * (n))
#define SPI_AGM_PHASE_DATA(n)     (0x30U + 4U * (n))

#define SPI_AGM_CTRL_START        BIT(0)
#define SPI_AGM_CTRL_DONE         BIT(1)
#define SPI_AGM_CTRL_ERROR        BIT(2)
#define SPI_AGM_CTRL_INT_CLR      BIT(3)
#define SPI_AGM_CTRL_PHASE_CNT_SHIFT 4
#define SPI_AGM_CTRL_PHASE_CNT_MASK  (0x7U << SPI_AGM_CTRL_PHASE_CNT_SHIFT)
#define SPI_AGM_CTRL_DMA_EN       BIT(8)
#define SPI_AGM_CTRL_WP_EN        BIT(9)
/* Little endian: PHASE_DATA byte 0 is clocked out first, which is what
 * makes a packed 32-bit word read as [b0, b1, b2, b3] on the wire. */
#define SPI_AGM_CTRL_ENDIAN       BIT(10)
#define SPI_AGM_CTRL_SCLK_DIV_SHIFT 12
#define SPI_AGM_CTRL_SCLK_DIV_MASK  (0xffU << SPI_AGM_CTRL_SCLK_DIV_SHIFT)
#define SPI_AGM_CTRL_INT_EN       BIT(20)
#define SPI_AGM_CTRL_RESET        BIT(31)

#define SPI_AGM_PHASE_ACTION_SHIFT 4
#define SPI_AGM_PHASE_ACTION_DUMMY (1U << SPI_AGM_PHASE_ACTION_SHIFT)
#define SPI_AGM_PHASE_ACTION_TX    (0U << SPI_AGM_PHASE_ACTION_SHIFT)
#define SPI_AGM_PHASE_ACTION_RX    (2U << SPI_AGM_PHASE_ACTION_SHIFT)
#define SPI_AGM_PHASE_ACTION_POLL  (3U << SPI_AGM_PHASE_ACTION_SHIFT)
#define SPI_AGM_PHASE_ERROR        BIT(2)
#define SPI_AGM_PHASE_BYTE_CNT_SHIFT 8
#define SPI_AGM_PHASE_BYTE_CNT_MASK  (0xfffU << SPI_AGM_PHASE_BYTE_CNT_SHIFT)
/* Per-phase byte counter is 12 bits wide. The DMA path uses this directly;
 * the register-fed (non-DMA) path stops at SPI_AGM_PHASE_BYTES (4) because
 * the phase data register is 32 bits. The public name for this limit,
 * SPI_AGM_MAX_PHASE_BYTES, lives in spi_agm.h -- it must not be defined a
 * second time here, because the duplicate trips -Werror in any build with
 * warnings-as-errors (twister sets that). */
#define SPI_AGM_PHASE_MODE_SHIFT 20
#define SPI_AGM_PHASE_MODE_MASK  (0x3U << SPI_AGM_PHASE_MODE_SHIFT)

/* A POLL phase keeps its settings in the phase's data register:
 * limit[31:24] mask[23:16] expect[15:8] read[7:0]. The read field comes back
 * 0x00 on this silicon, so the driver does not report
 * it -- see spi_agm_poll_status(). */
#define SPI_AGM_POLL_LIMIT_SHIFT   24
#define SPI_AGM_POLL_MASK_SHIFT    16
#define SPI_AGM_POLL_EXPECT_SHIFT  8

/* A phase's data register is 32 bits wide, so the direct (non-DMA) path
 * carries at most this many bytes in and out. */
/* One phase carries at most four bytes (32-bit data register); a transfer
 * may use up to SPI_AGM_MAX_PHASES of them. Both limits are public
 * (include/zephyr/drivers/spi/spi_agm.h) because spi_agm_transceive_phases()
 * enforces them and spi_agm_tx_phase_split() is the one place that has to
 * agree with that enforcement. */
#define SPI_AGM_PHASE_BYTES       SPI_AGM_MAX_TX_PHASE_BYTES
#define SPI_AGM_MAX_TX_BYTES      (SPI_AGM_MAX_PHASES * SPI_AGM_PHASE_BYTES)
/* The wait state the generic multiline wrapper inserts between the
 * command+address phase and the data phase, in SPI bytes (= 8 clocks
 * each). The common 0x3B/0x6B/0xEB fast-read family all want eight
 * dummy clocks; see the comment above spi_agm_transceive_multiline(). */
#define SPI_AGM_MULTILINE_DUMMY_BYTES 1U
/* The DMA path streams the TX byte stream through one register-fed header
 * phase (SPI_AGM_PHASE_BYTES) followed by a single DMA-fed phase that the
 * 12-bit counter caps at SPI_AGM_MAX_PHASE_BYTES. The total ceiling for one
 * transceive() TX frame is therefore the two added; that value is exposed
 * as SPI_AGM_MAX_TX_BYTES_TOTAL in spi_agm.h. */

/* Fastest divider is 2 (SCLK = SYSCLK / 2); the field holds the divider
 * with 0 meaning 256. */
#define SPI_AGM_DIV_MIN           2U
#define SPI_AGM_DIV_MAX           256U

/* A phase transfer is a few microseconds; this only exists so a gated or
 * mis-routed controller cannot hang the boot of a sample forever.
 *
 * Note the unit, because the name lies a little: spi_agm_wait_done() counts
 * *iterations*, and one iteration is a register read plus one k_busy_wait(1)
 * (~300 cycles = 1.5 us at 200 MHz). The
 * real ceiling is therefore ~150 ms, not the 100 ms the name suggests -- which
 * is fine for something whose only job is to not hang forever, and is why this
 * is a comment rather than a change to the loop. */
#define SPI_AGM_DONE_TIMEOUT_US   100000U

struct spi_agm_config {
	DEVICE_MMIO_ROM;
	uint32_t clock_freq;
	const struct pinctrl_dev_config *pcfg;
	/* SYS.APB_CLKENABLE bit that gates this controller (soc.c opens it at
	 * boot; the PM action closes and reopens it around suspend/resume). */
	uint32_t apb_clk_bit;
	/* Bitstream-declared multi-line capability (0=single, 1=dual, 2=quad).
	 * Comes from the optional `agm,spi-multiline` DT property; defaults to
	 * 0 so single-line bitstreams (the shipped vendor <build_dir>/zephyr/board.bin)
	 * keep their existing -ENOTSUP for SPI_LINES_DUAL/QUAD. */
	uint32_t multiline_cap;
	const struct device *dma;
	uint32_t dma_tx_channel;
	uint32_t dma_rx_channel;
	uint32_t dma_tx_request;
	uint32_t dma_rx_request;
};

/* One DMA transfer between memory and a phase data register. The controller
 * moves 32-bit words, so lengths must be multiples of four. */
struct spi_agm_dma_ctx {
	struct k_sem done;
	volatile int status;
};

/* What a prepared transfer needs: everything the engine was programmed with
 * and everything finishing it touches. Filled by spi_agm_prepare(). */
struct spi_agm_xfer {
	/* Non-NULL when the caller drives the chip select through a GPIO. */
	const struct spi_cs_control *cs;
	uint8_t phases;
	uint8_t rx_phase;
	size_t rx_len;
	size_t rx_skip;
	size_t rx_cap;
	bool dma_tx;
	bool dma_rx;
	struct spi_agm_dma_ctx tx_ctx;
	struct spi_agm_dma_ctx rx_ctx;
};

/* The interrupt-driven transfer that is currently running, if any. The SPI
 * API keeps the caller's buffers alive until the callback, so the buffer set
 * is just referenced. */
struct spi_agm_async {
	const struct device *dev;
	spi_callback_t callback;
	void *user_data;
	const struct spi_buf_set *rx_bufs;
	struct spi_agm_xfer xfer;
	struct k_work work;
	volatile bool busy;
};

struct spi_agm_data {
	DEVICE_MMIO_RAM;
	struct k_mutex lock;
	struct spi_agm_async async;
	/* Used by spi_agm_transceive_phases() for its RX phase. */
	struct spi_agm_dma_ctx phase_ctx;
	/* The DMA engine moves 32-bit words, so an RX transfer longer than
	 * four bytes is drained here first and then copied into the caller's
	 * (possibly non-contiguous, non-multiple-of-four) buffer list. */
	uint8_t rx_bounce[CONFIG_SPI_AGM_RX_BOUNCE_BYTES] __aligned(4);
	/* Same story in the other direction: a long TX whose buffer list is
	 * not a single, four-byte-multiple block (the spi-nor flash driver
	 * hands over command+address and data as two buffers) is gathered
	 * here before the DMAC reads it. */
	uint8_t tx_bounce[CONFIG_SPI_AGM_TX_BOUNCE_BYTES] __aligned(4);
};

static inline uint32_t spi_agm_read(const struct device *dev, uint32_t off)
{
	return sys_read32(DEVICE_MMIO_GET(dev) + off);
}

static inline void spi_agm_write(const struct device *dev, uint32_t off, uint32_t val)
{
	sys_write32(val, DEVICE_MMIO_GET(dev) + off);
}

/* A transfer that did not finish leaves the engine mid-frame (START still
 * set) and every later transfer then looks broken. */
static void spi_agm_soft_reset(const struct device *dev)
{
	spi_agm_write(dev, SPI_AGM_CTRL, SPI_AGM_CTRL_RESET);
	spi_agm_write(dev, SPI_AGM_CTRL, 0U);
}

#if defined(CONFIG_PM_DEVICE)
/* Gating this controller's SYS.APB_CLKENABLE bit stops its register clock: a
 * read then returns 0 and the engine forgets its configuration, which is why
 * the resume path re-runs the reset and the pin routing. soc.c opened the
 * gate at PRE_KERNEL_1; from here on the device owns it while idle. */
static void spi_agm_apb_gate(const struct device *dev, bool on)
{
	const struct spi_agm_config *config = dev->config;
	uint32_t reg = (uint32_t)(AGM_SYS_BASE + AGM_SYS_APB_CLKENABLE);

	if (on) {
		sys_set_bit(reg, config->apb_clk_bit);
	} else {
		sys_clear_bit(reg, config->apb_clk_bit);
	}
}

static int spi_agm_pm_action(const struct device *dev, enum pm_device_action action)
{
	const struct spi_agm_config *config = dev->config;

	switch (action) {
	case PM_DEVICE_ACTION_TURN_ON:
	case PM_DEVICE_ACTION_RESUME:
		spi_agm_apb_gate(dev, true);
		spi_agm_soft_reset(dev);

		return pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	case PM_DEVICE_ACTION_SUSPEND:
	case PM_DEVICE_ACTION_TURN_OFF:
		spi_agm_apb_gate(dev, false);

		return 0;
	default:
		return -ENOTSUP;
	}
}
#endif /* CONFIG_PM_DEVICE */

/* Keeping the device resumed for the duration of a transfer is what makes
 * the PM action safe: the register clock is only off while nobody is using
 * the controller. */
static int spi_agm_runtime_get(const struct device *dev)
{
	if (IS_ENABLED(CONFIG_PM_DEVICE_RUNTIME)) {
		return pm_device_runtime_get(dev);
	}

	return 0;
}

static void spi_agm_runtime_put(const struct device *dev)
{
	if (IS_ENABLED(CONFIG_PM_DEVICE_RUNTIME)) {
		(void)pm_device_runtime_put(dev);
	}
}

static uint32_t spi_agm_pack(const uint8_t *buf, size_t len)
{
	uint32_t word = 0U;

	for (size_t i = 0U; i < len; i++) {
		word |= (uint32_t)buf[i] << (8U * i);
	}

	return word;
}

static void spi_agm_unpack(uint32_t word, uint8_t *buf, size_t len)
{
	for (size_t i = 0U; i < len; i++) {
		buf[i] = (uint8_t)(word >> (8U * i));
	}
}

/* Hand the caller its RX bytes.
 *
 * The SPI API counts slots: byte i of the transfer is clocked with tx[i] on
 * MOSI (or filler when the request is shorter than the frame) and lands in
 * rx[i]. A full-duplex controller samples MISO in every one of those slots,
 * including the ones that carry the command. This engine clocks its TX
 * phases without sampling, so the first `skip` RX bytes -- the slots that
 * held the command -- cannot come from the bus; they are reported as 0xFF
 * and the captured stream fills the rest. */
static void spi_agm_rx_scatter(const struct spi_buf_set *rx_bufs, size_t rx_len, size_t skip,
			       const uint8_t *captured, size_t cap_len)
{
	size_t pos = 0U;
	size_t got = 0U;

	for (size_t i = 0U; (i < rx_bufs->count) && (pos < rx_len); i++) {
		uint8_t *dst = rx_bufs->buffers[i].buf;
		size_t chunk = MIN(rx_bufs->buffers[i].len, rx_len - pos);

		if (dst != NULL) {
			for (size_t j = 0U; j < chunk; j++) {
				if ((pos + j) < skip) {
					dst[j] = 0xFFU;
				} else if (got < cap_len) {
					dst[j] = captured[got++];
				} else {
					dst[j] = 0xFFU;
				}
			}
		}

		pos += chunk;
	}
}

static int spi_agm_set_frequency(const struct device *dev, uint32_t frequency)
{
	const struct spi_agm_config *config = dev->config;
	uint32_t div;

	if (frequency == 0U) {
		return -EINVAL;
	}

	for (div = SPI_AGM_DIV_MIN; div <= SPI_AGM_DIV_MAX; div <<= 1) {
		if ((config->clock_freq / div) <= frequency) {
			break;
		}
	}

	if (div > SPI_AGM_DIV_MAX) {
		/* Slower than SYSCLK / 256: the engine cannot go there. */
		return -EINVAL;
	}

	spi_agm_write(dev, SPI_AGM_CTRL, SPI_AGM_CTRL_ENDIAN |
		      ((div == SPI_AGM_DIV_MAX ? 0U : div) << SPI_AGM_CTRL_SCLK_DIV_SHIFT));

	return 0;
}

static void spi_agm_dma_cb(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	struct spi_agm_dma_ctx *ctx = user_data;

	ARG_UNUSED(dev);
	ARG_UNUSED(channel);

	ctx->status = status;
	k_sem_give(&ctx->done);
}

static int spi_agm_dma_start(const struct device *dev, uint32_t channel,
			     enum dma_channel_direction dir, uint32_t src, uint32_t dst, size_t len,
			     uint32_t request, struct spi_agm_dma_ctx *ctx)
{
	const struct spi_agm_config *config = dev->config;
	struct dma_block_config block = { 0 };
	struct dma_config cfg = { 0 };
	int ret;

	k_sem_init(&ctx->done, 0, 1);
	ctx->status = -1;

	block.source_address = src;
	block.dest_address = dst;
	block.block_size = len;
	block.source_addr_adj = (dir == MEMORY_TO_PERIPHERAL) ? DMA_ADDR_ADJ_INCREMENT
							      : DMA_ADDR_ADJ_NO_CHANGE;
	block.dest_addr_adj = (dir == MEMORY_TO_PERIPHERAL) ? DMA_ADDR_ADJ_NO_CHANGE
							    : DMA_ADDR_ADJ_INCREMENT;

	cfg.channel_direction = dir;
	cfg.source_data_size = 4U;
	cfg.dest_data_size = 4U;
	cfg.source_burst_length = 1U;
	cfg.dest_burst_length = 1U;
	cfg.block_count = 1U;
	cfg.head_block = &block;
	cfg.dma_callback = spi_agm_dma_cb;
	cfg.user_data = ctx;
	cfg.complete_callback_en = 1U;
	cfg.dma_slot = request;

	ret = dma_config(config->dma, channel, &cfg);
	if (ret < 0) {
		return ret;
	}

	return dma_start(config->dma, channel);
}

static int spi_agm_dma_wait(const struct device *dev, uint32_t channel,
			    struct spi_agm_dma_ctx *ctx)
{
	const struct spi_agm_config *config = dev->config;

	if (k_sem_take(&ctx->done, K_MSEC(1000)) != 0) {
		/* Abort and release the channel: a transfer whose peripheral
		 * never requests must not leave the channel busy forever.
		 *
		 * Also soft-reset the engine: a DMA transfer that never completed
		 * leaves it mid-communication (SPI_START still set), and on the
		 * A broken transfer poisons every later one -- a broken RX
		 * transfer made the following TX transfer look broken too. */
		(void)dma_stop(config->dma, channel);
		spi_agm_soft_reset(dev);

		return -ETIMEDOUT;
	}

	return ctx->status;
}

/* START a prepared transfer: the phase count (0 = one phase) and the DMA bit.
 * DMA (when enabled) applies to the last phase. No interrupt: see the async
 * entry point for why the engine's completion line is not used. */
static void spi_agm_start(const struct device *dev, uint8_t phases, bool dma_en)
{
	uint32_t ctrl = SPI_AGM_CTRL_START |
			((uint32_t)(phases - 1U) << SPI_AGM_CTRL_PHASE_CNT_SHIFT) |
			(dma_en ? SPI_AGM_CTRL_DMA_EN : 0U);

	spi_agm_write(dev, SPI_AGM_CTRL,
		      (spi_agm_read(dev, SPI_AGM_CTRL) &
		       ~(SPI_AGM_CTRL_PHASE_CNT_MASK | SPI_AGM_CTRL_DMA_EN | SPI_AGM_CTRL_INT_EN)) |
			      ctrl);
}

static int spi_agm_wait_done(const struct device *dev)
{
	uint32_t elapsed = 0U;

	while ((spi_agm_read(dev, SPI_AGM_CTRL) & SPI_AGM_CTRL_DONE) == 0U) {
		if (elapsed >= SPI_AGM_DONE_TIMEOUT_US) {
			return -ETIMEDOUT;
		}

		k_busy_wait(1);
		elapsed++;
	}

	if ((spi_agm_read(dev, SPI_AGM_CTRL) & SPI_AGM_CTRL_ERROR) != 0U) {
		return -EIO;
	}

	return 0;
}

static int spi_agm_run(const struct device *dev, uint8_t phases, bool dma_en)
{
	spi_agm_start(dev, phases, dma_en);

	return spi_agm_wait_done(dev);
}

static int spi_agm_check_config(const struct device *dev, const struct spi_config *spi_cfg)
{
	if (SPI_OP_MODE_GET(spi_cfg->operation) != SPI_OP_MODE_CONTROLLER) {
		return -ENOTSUP;
	}

	/* Mode 0 only: CPOL/CPHA live in the fabric patch, not in this IP. */
	if (SPI_MODE_GET(spi_cfg->operation) != 0U) {
		return -ENOTSUP;
	}

	if (SPI_WORD_SIZE_GET(spi_cfg->operation) != 8U) {
		return -ENOTSUP;
	}

	/* Multi-line mode is bitstream-gated: the IP itself is single-line, and
	 * it takes a fabric path to fan IO2/IO3 out to WP#/HOLD# on the flash.
	 * spi_quad_read on ~/spi_full_bitstream_97pad/ declares agm,spi-multiline
	 * = 2 and reaches the engine through the phase-list extension; here we
	 * refuse anything the bitstream has not declared. */
	const struct spi_agm_config *cfg = dev->config;
	uint32_t requested = spi_cfg->operation & SPI_LINES_MASK;

	/* SPI_LINES_DUAL/QUAD are 1U<<16 / 2U<<16; multiline_cap is 0/1/2.
	 * Reduce to the line count so the cap can be compared directly. */
	if ((requested >> 16) > cfg->multiline_cap) {
		return -ENOTSUP;
	}

	/* Everything below changes what the caller expects to see on the wire
	 * and the engine cannot do any of it, so refuse instead of clocking
	 * something else out:
	 *
	 *  - bit order is MSB-first (the data register is packed for that);
	 *  - the chip select is asserted by the engine for exactly one phase
	 *    run, so there is nothing to hold across calls (SPI_HOLD_ON_CS /
	 *    SPI_LOCK_ON, which spi_release() would end);
	 *  - the engine's CSN is active low. When CS is a GPIO the devicetree
	 *    flags decide the polarity, so the operation flag is redundant
	 *    there and only meaningful for a controller-driven CS. */
	if ((spi_cfg->operation & SPI_TRANSFER_LSB) != 0U) {
		return -ENOTSUP;
	}

	if ((spi_cfg->operation & (SPI_HOLD_ON_CS | SPI_LOCK_ON)) != 0U) {
		return -ENOTSUP;
	}

	if (((spi_cfg->operation & SPI_CS_ACTIVE_HIGH) != 0U) && !spi_cfg->cs.cs_is_gpio) {
		return -ENOTSUP;
	}

	return 0;
}

/* Undo whatever a half-prepared transfer left behind. */
static void spi_agm_abort(const struct device *dev, struct spi_agm_xfer *x)
{
	const struct spi_agm_config *config = dev->config;

	if (x->dma_tx) {
		(void)dma_stop(config->dma, config->dma_tx_channel);
	}

	if (x->dma_rx) {
		(void)dma_stop(config->dma, config->dma_rx_channel);
	}

	if (x->phases != 0U) {
		spi_agm_soft_reset(dev);
		x->phases = 0U;
	}

	if (x->cs != NULL) {
		(void)gpio_pin_set_dt(&x->cs->gpio, 0);
		x->cs = NULL;
	}
}

/* Program the phases, claim the chip select and arm the DMA. The engine is
 * not started here: the caller picks between waiting (spi_agm_run) and the
 * interrupt (SPI_AGM_CTRL_INT_EN). The caller's buffer sets have to stay
 * valid until the transfer is collected. */
static int spi_agm_prepare(const struct device *dev, const struct spi_config *spi_cfg,
			   const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs,
			   struct spi_agm_xfer *x)
{
	struct spi_agm_data *data = dev->data;
	const struct spi_agm_config *config = dev->config;
	size_t tx_len = 0U;
	size_t rx_len = 0U;
	uint8_t phases;
	uint32_t dma_tx_src = 0U;
	size_t dma_tx_len = 0U;
	const uint8_t *tx_src = NULL;
	int ret;

	*x = (struct spi_agm_xfer){ 0 };

	ret = spi_agm_check_config(dev, spi_cfg);
	if (ret < 0) {
		return ret;
	}

	if ((tx_bufs == NULL) || (tx_bufs->count == 0U) || (tx_bufs->buffers == NULL)) {
		/* Constraint 1: the first phase must be TX. Without a fabric
		 * full-duplex path there is nothing to clock an RX phase. */
		return -ENOTSUP;
	}

	for (size_t i = 0U; i < tx_bufs->count; i++) {
		tx_len += tx_bufs->buffers[i].len;
	}

	if ((rx_bufs != NULL) && (rx_bufs->buffers != NULL)) {
		for (size_t i = 0U; i < rx_bufs->count; i++) {
			rx_len += rx_bufs->buffers[i].len;
		}
	}

	/* The TX phases hold the first tx_len slots of the frame. The engine
	 * cannot sample MISO there, so only the slots that follow them can be
	 * read back; see spi_agm_rx_scatter(). */
	x->rx_len = rx_len;
	x->rx_skip = MIN(tx_len, rx_len);
	x->rx_cap = rx_len - x->rx_skip;

	x->dma_tx = (tx_len > SPI_AGM_MAX_TX_BYTES);
	x->dma_rx = (x->rx_cap > SPI_AGM_PHASE_BYTES);

	if ((rx_len > 0U) && (x->rx_cap == 0U)) {
		/* Every requested RX byte sits inside the TX window, i.e. the
		 * caller expects a full-duplex controller to sample MISO while
		 * the command is clocked out. This engine has no RX phase for
		 * that: extend the frame with dummy TX bytes instead. */
		return -ENOTSUP;
	}

	if (x->dma_tx) {
		/* The DMA moves 32-bit words out of memory; the first word still
		 * comes from the phase data register. Verified with the fabric
		 * loopback (samples/spi_loopback): a 36-byte transfer built this
		 * way puts exactly the requested bytes on MOSI. */
		if (config->dma == NULL) {
			/* No `dmas` on this node: the register-fed paths still
			 * work, the streaming ones do not. */
			return -ENOTSUP;
		}

		if (tx_len > SPI_AGM_MAX_TX_BYTES_TOTAL) {
			/* Four bytes go out of the data register and the rest out of
			 * the phase's count field, which is only 12 bits wide. */
			return -ENOTSUP;
		}

		if ((tx_bufs->count == 1U) && ((tx_len % 4U) == 0U) &&
		    (tx_bufs->buffers[0].buf != NULL)) {
			/* The DMAC can read the caller's buffer as it is. */
			tx_src = tx_bufs->buffers[0].buf;
		} else {
			/* Gather the buffer list (and any partial trailing word)
			 * into the bounce buffer; the engine clocks tx_len bytes,
			 * the padding only exists so the DMAC has whole words to
			 * move. */
			size_t padded = ROUND_UP(tx_len, sizeof(uint32_t));
			size_t copied = 0U;

			if (padded > sizeof(data->tx_bounce)) {
				return -EINVAL;
			}

			for (size_t i = 0U; (i < tx_bufs->count) && (copied < tx_len); i++) {
				size_t chunk = MIN(tx_bufs->buffers[i].len, tx_len - copied);

				if (tx_bufs->buffers[i].buf != NULL) {
					memcpy(&data->tx_bounce[copied],
					       tx_bufs->buffers[i].buf, chunk);
				}

				copied += chunk;
			}

			memset(&data->tx_bounce[tx_len], 0xFFU, padded - tx_len);
			tx_src = data->tx_bounce;
		}

		dma_tx_src = (uint32_t)(uintptr_t)tx_src + SPI_AGM_PHASE_BYTES;
		dma_tx_len = tx_len - SPI_AGM_PHASE_BYTES;
	}

	if (x->dma_tx && (rx_len > 0U)) {
		/* DMA applies to the last phase only, so a DMA-fed TX phase
		 * cannot be followed by an RX phase: a long TX is TX-only. */
		return -ENOTSUP;
	}

	if (x->dma_rx) {
		if (config->dma == NULL) {
			return -ENOTSUP;
		}

		if ((rx_bufs == NULL) || (rx_bufs->count == 0U) ||
		    (ROUND_UP(x->rx_cap, sizeof(uint32_t)) > sizeof(data->rx_bounce))) {
			return -EINVAL;
		}

		if (x->rx_cap > SPI_AGM_MAX_PHASE_BYTES) {
			/* The RX phase carries its byte count in the same 12-bit
			 * field as the TX ones. */
			return -ENOTSUP;
		}
	}

	if (!x->dma_tx && !x->dma_rx &&
	    ((((tx_len + SPI_AGM_PHASE_BYTES - 1U) / SPI_AGM_PHASE_BYTES) +
	      (x->rx_cap > 0U ? 1U : 0U)) > SPI_AGM_MAX_PHASES)) {
		return -ENOTSUP;
	}

	ret = spi_agm_set_frequency(dev, spi_cfg->frequency);
	if (ret < 0) {
		return ret;
	}

	if ((spi_cfg->cs.gpio.port != NULL) && spi_cfg->cs.cs_is_gpio) {
		ret = gpio_pin_configure_dt(&spi_cfg->cs.gpio, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			return ret;
		}

		x->cs = &spi_cfg->cs;

		if (spi_cfg->cs.delay != 0U) {
			k_busy_wait(spi_cfg->cs.delay);
		}

		ret = gpio_pin_set_dt(&spi_cfg->cs.gpio, 1);
		if (ret < 0) {
			spi_agm_abort(dev, x);
			return ret;
		}
	}

	phases = 0U;

	/* TX phases: four bytes each, in order. A phase's data register is
	 * both the source for TX and the sink for RX, so the last TX phase's
	 * index plus one is where the RX phase goes. */
	if (x->dma_tx) {
		spi_agm_write(dev, SPI_AGM_PHASE_CTRL(phases),
			      SPI_AGM_PHASE_ACTION_TX |
				      ((uint32_t)SPI_AGM_PHASE_BYTES
				       << SPI_AGM_PHASE_BYTE_CNT_SHIFT));
		spi_agm_write(dev, SPI_AGM_PHASE_DATA(phases),
			      spi_agm_pack(tx_src, SPI_AGM_PHASE_BYTES));
		phases++;

		spi_agm_write(dev, SPI_AGM_PHASE_CTRL(phases),
			      SPI_AGM_PHASE_ACTION_TX |
				      ((uint32_t)dma_tx_len << SPI_AGM_PHASE_BYTE_CNT_SHIFT));
		phases++;
	} else if (tx_len > 0U) {
		uint8_t tx[SPI_AGM_MAX_TX_BYTES] = { 0 };
		size_t copied = 0U;

		for (size_t i = 0U; (i < tx_bufs->count) && (copied < tx_len); i++) {
			size_t chunk = MIN(tx_bufs->buffers[i].len, tx_len - copied);

			if (tx_bufs->buffers[i].buf != NULL) {
				memcpy(&tx[copied], tx_bufs->buffers[i].buf, chunk);
			}

			copied += chunk;
		}

		for (size_t off = 0U; off < tx_len; off += SPI_AGM_PHASE_BYTES) {
			size_t chunk = MIN((size_t)SPI_AGM_PHASE_BYTES, tx_len - off);

			spi_agm_write(dev, SPI_AGM_PHASE_CTRL(phases),
				      SPI_AGM_PHASE_ACTION_TX |
					      ((uint32_t)chunk << SPI_AGM_PHASE_BYTE_CNT_SHIFT));
			spi_agm_write(dev, SPI_AGM_PHASE_DATA(phases), spi_agm_pack(&tx[off], chunk));
			phases++;
		}
	}

	/* Last phase: RX (the slots the TX phases did not already clock).
	 * Constraint 3 says nothing clocks after it. When it is longer than
	 * the data register, the RX phase streams its bytes out through that
	 * register with DMA. */
	if (x->rx_cap > 0U) {
		x->rx_phase = phases;
		spi_agm_write(dev, SPI_AGM_PHASE_CTRL(phases),
			      SPI_AGM_PHASE_ACTION_RX |
				      ((uint32_t)x->rx_cap << SPI_AGM_PHASE_BYTE_CNT_SHIFT));
		phases++;
	}

	/* The DMA channels follow the engine (and must be armed before it
	 * starts): TX feeds PHASE_DATA, RX drains it. */
	if (x->dma_tx) {
		ret = spi_agm_dma_start(dev, config->dma_tx_channel, MEMORY_TO_PERIPHERAL,
					dma_tx_src,
					(uint32_t)(DEVICE_MMIO_GET(dev) + SPI_AGM_PHASE_DATA(1)),
					dma_tx_len, config->dma_tx_request, &x->tx_ctx);
		if (ret < 0) {
			spi_agm_abort(dev, x);
			return ret;
		}
	}

	if (x->dma_rx) {
		ret = spi_agm_dma_start(dev, config->dma_rx_channel, PERIPHERAL_TO_MEMORY,
					(uint32_t)(DEVICE_MMIO_GET(dev) +
						   SPI_AGM_PHASE_DATA(x->rx_phase)),
					(uint32_t)(uintptr_t)data->rx_bounce,
					ROUND_UP(x->rx_cap, sizeof(uint32_t)),
					config->dma_rx_request, &x->rx_ctx);
		if (ret < 0) {
			spi_agm_abort(dev, x);
			return ret;
		}
	}

	x->phases = phases;

	return 0;
}

/* Finish a transfer: stop the DMA, copy the RX bytes out and release the
 * chip select. `status` is what the engine reported (0 = it finished).
 *
 * The interrupt handler cannot wait for the DMAC's completion semaphore, but
 * it does not have to: SPI_DONE means "SPI transmission and DMA transmission
 * ... have all ended". */
static int spi_agm_collect(const struct device *dev, const struct spi_buf_set *rx_bufs,
			   struct spi_agm_xfer *x, int status, bool from_isr)
{
	const struct spi_agm_config *config = dev->config;
	struct spi_agm_data *data = dev->data;
	int ret = status;

	/* Do not wait for the DMAC's terminal count on the TX side: with the
	 * peripheral as flow controller (the only configuration in which the
	 * engine's TX request is served) the DMAC does not raise one. */
	if (x->dma_tx) {
		(void)dma_stop(config->dma, config->dma_tx_channel);
	}

	if ((ret == 0) && x->dma_rx) {
		if (from_isr) {
			(void)dma_stop(config->dma, config->dma_rx_channel);
		} else {
			ret = spi_agm_dma_wait(dev, config->dma_rx_channel, &x->rx_ctx);
		}
	}

	if (ret < 0) {
		spi_agm_abort(dev, x);
		return ret;
	}

	if ((x->rx_len > 0U) && x->dma_rx) {
		spi_agm_rx_scatter(rx_bufs, x->rx_len, x->rx_skip, data->rx_bounce, x->rx_cap);
	} else if ((x->rx_cap > 0U) && !x->dma_rx) {
		/* The RX bytes come back in the RX phase's data register,
		 * little endian like the TX side. */
		uint8_t rx[SPI_AGM_PHASE_BYTES] = { 0 };

		spi_agm_unpack(spi_agm_read(dev, SPI_AGM_PHASE_DATA(x->rx_phase)), rx, x->rx_cap);
		spi_agm_rx_scatter(rx_bufs, x->rx_len, x->rx_skip, rx, x->rx_cap);
	}

	if (x->cs != NULL) {
		(void)gpio_pin_set_dt(&x->cs->gpio, 0);
		x->cs = NULL;
	}

	return ret;
}

/* Translate a generic-API request that asked for SPI_LINES_DUAL/QUAD into
 * the phase-list extension: one TX phase on a single line for the command
 * and address, one DUMMY phase for the wait states the flash needs before
 * the data phase, and one RX phase on the requested line mode.
 *
 * Dummy count: the common dual/quad-read protocols (W25Q16 0x3B/0x6B,
 * W25Q16 0xEB quad-I/O, Macronix MX25L 0xEB) want eight dummy clocks
 * between the address and the data window. On SPI that is one byte of
 * filler (8 clocks/byte). The previous implementation computed the
 * dummy byte count as `8 - cmd_len` so the cmd+addr+dummy total came
 * out to eight bytes; that happened to fit 0xEB (1 cmd + 4 addr + 3
 * dummy) but undershot every other protocol (e.g. 0x3B/0x6B got 4
 * dummy clocks instead of 8). One byte of dummy is the correct wait
 * state for the 0x3B/0x6B/0xEB family of fast-read commands.
 *
 * Devices with a non-standard dummy count have to use
 * spi_agm_transceive_phases() directly.
 *
 * Constraint: the command+address fits in one 32-bit PHASE_DATA register
 * (1..SPI_AGM_PHASE_BYTES bytes), the data phase fits in one phase
 * (1..SPI_AGM_MAX_PHASE_BYTES bytes), and both buffer lists are a single
 * buffer. Anything more complex returns -ENOTSUP and the caller can fall
 * back to the phase-list extension.
 */
static int spi_agm_transceive_multiline(const struct device *dev, const struct spi_config *spi_cfg,
					const struct spi_buf_set *tx_bufs,
					const struct spi_buf_set *rx_bufs)
{
	const struct spi_agm_config *cfg = dev->config;
	uint32_t requested = spi_cfg->operation & SPI_LINES_MASK;
	uint8_t cmd_phase_lines = SPI_AGM_LINES_SINGLE;
	uint8_t rx_phase_lines;
	uint8_t cmd[SPI_AGM_PHASE_BYTES] = { 0 };
	size_t cmd_len = 0U;
	size_t rx_len = 0U;
	size_t dummy_bytes = 0U;
	struct spi_agm_phase phases[3];
	uint8_t phase_count = 0U;

	if (requested == 0U) {
		return -ENOTSUP;
	}

	if ((requested >> 16) > cfg->multiline_cap) {
		return -ENOTSUP;
	}

	if ((tx_bufs == NULL) || (tx_bufs->count != 1U) || (tx_bufs->buffers == NULL)) {
		return -ENOTSUP;
	}

	if ((rx_bufs == NULL) || (rx_bufs->count != 1U) || (rx_bufs->buffers == NULL) ||
	    (rx_bufs->buffers[0].buf == NULL)) {
		return -ENOTSUP;
	}

	cmd_len = tx_bufs->buffers[0].len;
	rx_len = rx_bufs->buffers[0].len;

	if ((cmd_len == 0U) || (cmd_len > SPI_AGM_PHASE_BYTES)) {
		return -ENOTSUP;
	}

	if ((rx_len == 0U) || (rx_len > SPI_AGM_MAX_PHASE_BYTES)) {
		return -ENOTSUP;
	}

	if (rx_len > SPI_AGM_MAX_PHASE_BYTES - SPI_AGM_MULTILINE_DUMMY_BYTES) {
		/* SPI_AGM_MULTILINE_DUMMY_BYTES of dummy plus rx_len data bytes
		 * must fit in one phase. (The hardware split across two RX
		 * phases is not available -- see the comment at the top of
		 * the file.) */
		return -ENOTSUP;
	}

	if (tx_bufs->buffers[0].buf != NULL) {
		memcpy(cmd, tx_bufs->buffers[0].buf, cmd_len);
	}

	switch (requested) {
	case SPI_LINES_DUAL:
		rx_phase_lines = SPI_AGM_LINES_DUAL;
		break;
	case SPI_LINES_QUAD:
		rx_phase_lines = SPI_AGM_LINES_QUAD;
		break;
	default:
		return -ENOTSUP;
	}

	phases[phase_count].lines = cmd_phase_lines;
	phases[phase_count].dummy = false;
	phases[phase_count].len = (uint16_t)cmd_len;
	phases[phase_count].tx = cmd;
	phases[phase_count].rx = NULL;
	phase_count++;

	dummy_bytes = SPI_AGM_MULTILINE_DUMMY_BYTES;

	if (dummy_bytes > 0U) {
		phases[phase_count].lines = SPI_AGM_LINES_SINGLE;
		phases[phase_count].dummy = true;
		phases[phase_count].len = (uint16_t)dummy_bytes;
		phases[phase_count].tx = NULL;
		phases[phase_count].rx = NULL;
		phase_count++;
	}

	phases[phase_count].lines = rx_phase_lines;
	phases[phase_count].dummy = false;
	phases[phase_count].len = (uint16_t)rx_len;
	phases[phase_count].tx = NULL;
	phases[phase_count].rx = rx_bufs->buffers[0].buf;
	phase_count++;

	return spi_agm_transceive_phases(dev, spi_cfg, phases, phase_count);
}

static int spi_agm_transceive(const struct device *dev, const struct spi_config *spi_cfg,
			      const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs)
{
	struct spi_agm_data *data = dev->data;
	struct spi_agm_xfer x;
	int ret;

	/* Multi-line requests go through the phase engine (spi_agm_check_config
	 * would otherwise refuse SPI_LINES_DUAL/QUAD because the bitstream
	 * declares agm,spi-multiline only when its fabric routes IO2/IO3 to the
	 * flash's WP#/HOLD# pins). Hand the whole thing off to the extension;
	 * it handles runtime + locking on its own. */
	if (spi_cfg != NULL) {
		uint32_t lines = spi_cfg->operation & SPI_LINES_MASK;

		if ((lines == SPI_LINES_DUAL) || (lines == SPI_LINES_QUAD)) {
			return spi_agm_transceive_multiline(dev, spi_cfg, tx_bufs, rx_bufs);
		}
	}

	/* A NULL config is refused rather than "use whatever was configured
	 * last": spi_agm_prepare() dereferences it for the frame format, the
	 * chip-select spec and the DMA handles, and Zephyr's SPI API documents
	 * `config` as required. */
	if (spi_cfg == NULL) {
		return -EINVAL;
	}

	ret = spi_agm_runtime_get(dev);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if (data->async.busy) {
		ret = -EBUSY;
		goto out_unlock;
	}

	ret = spi_agm_prepare(dev, spi_cfg, tx_bufs, rx_bufs, &x);
	if (ret == 0) {
		ret = spi_agm_run(dev, x.phases, x.dma_tx || x.dma_rx);
		ret = spi_agm_collect(dev, rx_bufs, &x, ret, false);
	}

out_unlock:

	k_mutex_unlock(&data->lock);

	spi_agm_runtime_put(dev);

	return ret;
}

static int spi_agm_release(const struct device *dev, const struct spi_config *spi_cfg)
{
	struct spi_agm_data *data = dev->data;

	ARG_UNUSED(spi_cfg);

	if (data->async.busy) {
		return -EBUSY;
	}

	return 0;
}

#if defined(CONFIG_SPI_ASYNC)
/* Finish an async transfer from the system workqueue: wait for the engine,
 * collect the RX bytes, reset and report.
 *
 * Why not the engine's interrupt:, SPI_DONE_INT latches
 * high on the first completion and is not released by clearing SPI_DONE (the
 * manual's clear condition), by writing the whole CTRL register to 0, or by
 * the controller's soft reset -- a later transfer with INT_EN set and DONE
 * high produced no new interrupt at all, so only the first submission would
 * ever complete. Waiting in a work item instead leaves the caller unblocked
 * and works for every submission (it costs the workqueue thread the wait). */
static void spi_agm_async_finish(struct k_work *work)
{
	struct spi_agm_async *a = CONTAINER_OF(work, struct spi_agm_async, work);
	const struct device *dev = a->dev;
	struct spi_agm_data *data = dev->data;
	spi_callback_t callback = a->callback;
	void *user_data = a->user_data;
	int status;

	status = spi_agm_wait_done(dev);
	status = spi_agm_collect(dev, a->rx_bufs, &a->xfer, status, false);
	spi_agm_soft_reset(dev);

	/* Publish "idle" under the same lock the submission paths take, so a
	 * concurrent transceive() cannot observe a half-cleared async state.
	 * The callback runs *after* the unlock on purpose: a callback is
	 * allowed to re-enter the driver (e.g. submit the next transfer), and
	 * the mutex is not recursive. */
	k_mutex_lock(&data->lock, K_FOREVER);
	a->callback = NULL;
	a->user_data = NULL;
	a->rx_bufs = NULL;
	a->busy = false;
	k_mutex_unlock(&data->lock);

	if (callback != NULL) {
		callback(dev, status, user_data);
	}

	/* Release the submit's reference last, so a callback that uses the
	 * device again nests inside it. */
	spi_agm_runtime_put(dev);
}

static int spi_agm_transceive_async(const struct device *dev, const struct spi_config *spi_cfg,
				    const struct spi_buf_set *tx_bufs,
				    const struct spi_buf_set *rx_bufs, spi_callback_t callback,
				    void *user_data)
{
	struct spi_agm_data *data = dev->data;
	int ret;

	if (callback == NULL) {
		return -EINVAL;
	}

	/* Held until the work item finishes, so the device cannot suspend while
	 * the engine is clocking. */
	ret = spi_agm_runtime_get(dev);
	if (ret < 0) {
		return ret;
	}

	/* Same lock as the synchronous entry points and the phase-list
	 * extensions: `async.busy` is checked *and* set under it, and the
	 * preparation below reprograms PHASE_CTRL/PHASE_DATA -- two concurrent
	 * submissions must not interleave there. */
	k_mutex_lock(&data->lock, K_FOREVER);

	if (data->async.busy) {
		ret = -EBUSY;
		goto out_unlock;
	}

	/* Clear whatever the previous transfer left in CTRL -- DONE, INT_EN and
	 * the rest. The engine's completion line latches: writing 0 to the
	 * SPI_DONE bit alone does not release it, and a transfer that starts
	 * while it is still high never produces a new interrupt edge. */
	spi_agm_write(dev, SPI_AGM_CTRL, 0U);

	ret = spi_agm_prepare(dev, spi_cfg, tx_bufs, rx_bufs, &data->async.xfer);
	if (ret < 0) {
		goto out_unlock;
	}

	data->async.callback = callback;
	data->async.user_data = user_data;
	data->async.rx_bufs = rx_bufs;
	data->async.dev = dev;
	data->async.busy = true;

	spi_agm_start(dev, data->async.xfer.phases,
		      data->async.xfer.dma_tx || data->async.xfer.dma_rx);
	k_work_submit(&data->async.work);

out_unlock:
	k_mutex_unlock(&data->lock);

	if (ret < 0) {
		spi_agm_runtime_put(dev);
	}

	return ret;
}
#endif /* CONFIG_SPI_ASYNC */

int spi_agm_transceive_phases(const struct device *dev, const struct spi_config *spi_cfg,
			      const struct spi_agm_phase *phases, size_t count)
{
	struct spi_agm_data *data;
	const struct spi_agm_config *config = dev->config;
	bool rx_dma = false;
	size_t rx_len = 0U;
	size_t rx_index = 0U;
	int ret;

	if ((dev == NULL) || (dev->api != &spi_agm_api)) {
		return -ENODEV;
	}

	if ((phases == NULL) || (count == 0U) || (count > SPI_AGM_MAX_PHASES)) {
		return -EINVAL;
	}

	ret = spi_agm_check_config(dev, spi_cfg);
	if (ret < 0) {
		return ret;
	}

	for (size_t i = 0U; i < count; i++) {
		const struct spi_agm_phase *p = &phases[i];

		if ((p->len == 0U) || (p->len > SPI_AGM_MAX_PHASE_BYTES) ||
		    (p->lines > SPI_AGM_LINES_QUAD)) {
			return -EINVAL;
		}

		if (p->rx != NULL) {
			/* RX is the last phase, and only one of them. */
			if ((i != (count - 1U)) || (p->dummy) || (rx_len != 0U)) {
				return -EINVAL;
			}

			rx_len = p->len;
			rx_index = i;
		} else if (!p->dummy && ((p->tx == NULL) || (p->len > SPI_AGM_PHASE_BYTES))) {
			/* A data phase has to come out of the data register. */
			return -EINVAL;
		}
	}

	if ((count > 0U) && (phases[0].rx != NULL)) {
		/* Constraint 1: the engine cannot start with RX. */
		return -ENOTSUP;
	}

	rx_dma = (rx_len > SPI_AGM_PHASE_BYTES);

	if (rx_dma) {
		if (config->dma == NULL) {
			return -ENOTSUP;
		}

		if (ROUND_UP(rx_len, sizeof(uint32_t)) >
		    sizeof(((struct spi_agm_data *)0)->rx_bounce)) {
			return -EINVAL;
		}
	}

	data = dev->data;

	ret = spi_agm_runtime_get(dev);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if (data->async.busy) {
		ret = -EBUSY;
		goto out_unlock;
	}

	ret = spi_agm_set_frequency(dev, spi_cfg->frequency);
	if (ret < 0) {
		goto out_unlock;
	}

	for (size_t i = 0U; i < count; i++) {
		const struct spi_agm_phase *p = &phases[i];
		uint32_t action;
		uint32_t ctrl;

		if (p->rx != NULL) {
			action = SPI_AGM_PHASE_ACTION_RX;
		} else if (p->dummy) {
			action = SPI_AGM_PHASE_ACTION_DUMMY;
		} else {
			action = SPI_AGM_PHASE_ACTION_TX;
		}

		ctrl = action | ((uint32_t)p->len << SPI_AGM_PHASE_BYTE_CNT_SHIFT);

		if (p->lines != SPI_AGM_LINES_SINGLE) {
			ctrl |= (uint32_t)p->lines << SPI_AGM_PHASE_MODE_SHIFT;
		}

		spi_agm_write(dev, SPI_AGM_PHASE_CTRL(i), ctrl);

		if (action == SPI_AGM_PHASE_ACTION_TX) {
			uint8_t frame[SPI_AGM_PHASE_BYTES] = { 0 };

			memcpy(frame, p->tx, p->len);
			spi_agm_write(dev, SPI_AGM_PHASE_DATA(i),
				      spi_agm_pack(frame, p->len));
		} else {
			spi_agm_write(dev, SPI_AGM_PHASE_DATA(i), 0U);
		}
	}

	if (rx_dma) {
		ret = spi_agm_dma_start(dev, config->dma_rx_channel, PERIPHERAL_TO_MEMORY,
					(uint32_t)(DEVICE_MMIO_GET(dev) +
						   SPI_AGM_PHASE_DATA(rx_index)),
					(uint32_t)(uintptr_t)data->rx_bounce,
					ROUND_UP(rx_len, sizeof(uint32_t)),
					config->dma_rx_request, &data->phase_ctx);
		if (ret < 0) {
			spi_agm_soft_reset(dev);

			goto out_unlock;
		}
	}

	ret = spi_agm_run(dev, (uint8_t)count, rx_dma);

	if ((ret == 0) && rx_dma) {
		ret = spi_agm_dma_wait(dev, config->dma_rx_channel, &data->phase_ctx);
	}

	if (ret < 0) {
		if (rx_dma) {
			(void)dma_stop(config->dma, config->dma_rx_channel);
		}

		spi_agm_soft_reset(dev);

		goto out_unlock;
	}

	if (rx_len > 0U) {
		if (rx_dma) {
			memcpy(phases[rx_index].rx, data->rx_bounce, rx_len);
		} else {
			uint8_t rx[SPI_AGM_PHASE_BYTES] = { 0 };

			spi_agm_unpack(spi_agm_read(dev, SPI_AGM_PHASE_DATA(rx_index)), rx,
				       rx_len);
			memcpy(phases[rx_index].rx, rx, rx_len);
		}
	}

out_unlock:
	k_mutex_unlock(&data->lock);
	spi_agm_runtime_put(dev);

	return ret;
}

DEVICE_API(spi, spi_agm_api) = {
	.transceive = spi_agm_transceive,
	.release = spi_agm_release,
#if defined(CONFIG_SPI_ASYNC)
	.transceive_async = spi_agm_transceive_async,
#endif
};

int spi_agm_poll_status(const struct device *dev, const struct spi_config *spi_cfg,
			const uint8_t *cmd, size_t cmd_len, uint8_t mask, uint8_t expect,
			uint8_t limit)
{
	struct spi_agm_data *data;
	uint8_t frame[SPI_AGM_PHASE_BYTES] = { 0 };
	uint32_t phase1;
	int ret;

	if ((dev == NULL) || (dev->api != &spi_agm_api)) {
		return -ENODEV;
	}

	if ((cmd == NULL) || (cmd_len == 0U) || (cmd_len > SPI_AGM_PHASE_BYTES)) {
		return -EINVAL;
	}

	ret = spi_agm_check_config(dev, spi_cfg);
	if (ret < 0) {
		return ret;
	}

	data = dev->data;

	ret = spi_agm_runtime_get(dev);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if (data->async.busy) {
		ret = -EBUSY;
		k_mutex_unlock(&data->lock);
		spi_agm_runtime_put(dev);

		return ret;
	}

	ret = spi_agm_set_frequency(dev, spi_cfg->frequency);
	if (ret < 0) {
		goto out_unlock;
	}

	/* Phase 0: the command, exactly as a normal TX phase. */
	memcpy(frame, cmd, cmd_len);
	spi_agm_write(dev, SPI_AGM_PHASE_CTRL(0),
		      SPI_AGM_PHASE_ACTION_TX |
			      ((uint32_t)cmd_len << SPI_AGM_PHASE_BYTE_CNT_SHIFT));
	spi_agm_write(dev, SPI_AGM_PHASE_DATA(0), spi_agm_pack(frame, cmd_len));

	/* Phase 1: poll. The byte count is not used here; the phase's data
	 * register carries limit/mask/expect. */
	spi_agm_write(dev, SPI_AGM_PHASE_CTRL(1), SPI_AGM_PHASE_ACTION_POLL);
	spi_agm_write(dev, SPI_AGM_PHASE_DATA(1),
		      ((uint32_t)((limit == 0U) ? 0xffU : limit) << SPI_AGM_POLL_LIMIT_SHIFT) |
			      ((uint32_t)mask << SPI_AGM_POLL_MASK_SHIFT) |
			      ((uint32_t)expect << SPI_AGM_POLL_EXPECT_SHIFT));

	ret = spi_agm_run(dev, 2U, false);

	phase1 = spi_agm_read(dev, SPI_AGM_PHASE_CTRL(1));

	if ((ret == -EIO) && ((phase1 & SPI_AGM_PHASE_ERROR) != 0U)) {
		/* The engine ran out of attempts: a timeout from the caller's
		 * point of view. */
		ret = -ETIMEDOUT;
	}

	if (ret < 0) {
		spi_agm_soft_reset(dev);
	}

out_unlock:
	k_mutex_unlock(&data->lock);
	spi_agm_runtime_put(dev);

	return ret;
}

int spi_agm_clock_dummy(const struct device *dev, const struct spi_config *spi_cfg, size_t bytes)
{
	struct spi_agm_data *data;
	int ret;

	if ((dev == NULL) || (dev->api != &spi_agm_api)) {
		return -ENODEV;
	}

	if ((bytes == 0U) || (bytes > SPI_AGM_MAX_PHASE_BYTES)) {
		return -EINVAL;
	}

	ret = spi_agm_check_config(dev, spi_cfg);
	if (ret < 0) {
		return ret;
	}

	data = dev->data;

	ret = spi_agm_runtime_get(dev);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if (data->async.busy) {
		ret = -EBUSY;
		k_mutex_unlock(&data->lock);
		spi_agm_runtime_put(dev);

		return ret;
	}

	ret = spi_agm_set_frequency(dev, spi_cfg->frequency);
	if (ret < 0) {
		goto out_unlock;
	}

	/* One DUMMY TX phase: no data register to program, no DMA. */
	spi_agm_write(dev, SPI_AGM_PHASE_CTRL(0),
		      SPI_AGM_PHASE_ACTION_DUMMY |
			      ((uint32_t)bytes << SPI_AGM_PHASE_BYTE_CNT_SHIFT));

	ret = spi_agm_run(dev, 1U, false);

	if (ret < 0) {
		spi_agm_soft_reset(dev);
	}

out_unlock:
	k_mutex_unlock(&data->lock);
	spi_agm_runtime_put(dev);

	return ret;
}

/* Shared CS management for the long helpers: drive CS low, hand control back
 * to the helper for the inner loop, then drive CS high again. The engine has
 * no register to hold CS across phase lists, so the helpers above here have
 * to do it through a GPIO.
 *
 * spi_agm_transceive_phases() never touches the CS GPIO itself -- it goes
 * straight to the extension path, which assumes CS has already been dealt
 * with -- so what the helper does here is the only CS assertion those inner
 * calls see.
 */
static int spi_agm_long_cs_hold(const struct device *dev, const struct spi_config *spi_cfg,
				int (*body)(const struct device *, const struct spi_config *,
					    void *), void *ctx)
{
	int ret;

	if (!spi_cfg->cs.cs_is_gpio || (spi_cfg->cs.gpio.port == NULL)) {
		return -ENOTSUP;
	}

	ret = gpio_pin_configure_dt(&spi_cfg->cs.gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	if (spi_cfg->cs.delay != 0U) {
		k_busy_wait(spi_cfg->cs.delay);
	}

	ret = gpio_pin_set_dt(&spi_cfg->cs.gpio, 1);
	if (ret < 0) {
		return ret;
	}

	ret = body(dev, spi_cfg, ctx);

	(void)gpio_pin_set_dt(&spi_cfg->cs.gpio, 0);

	return ret;
}

/* Context for spi_agm_write_long's body callback. */
struct spi_agm_write_long_ctx {
	const uint8_t *tx;
	size_t tx_len;
	size_t offset;
};

/* One iteration of the write_long loop: up to SPI_AGM_MAX_PHASES
 * register-fed TX phases, i.e. SPI_AGM_MAX_TX_BYTES (32) bytes per phase
 * list.
 *
 * This is deliberately NOT one phase of SPI_AGM_MAX_PHASE_BYTES: without DMA
 * a TX phase is fed from the 32-bit data register, so the phase engine accepts
 * at most SPI_AGM_PHASE_BYTES (4) bytes per TX phase -- spi_agm_transceive_phases()
 * rejects anything longer with -EINVAL (see its validation loop). Slicing into
 * 4-byte phases and packing eight of them per call keeps the whole 4 KiB in the
 * caller's single CS window while staying inside what the engine can express. */
static int spi_agm_write_long_body(const struct device *dev, const struct spi_config *spi_cfg,
				   void *vctx)
{
	struct spi_agm_write_long_ctx *ctx = vctx;
	struct spi_agm_phase phases[SPI_AGM_MAX_PHASES];

	while (ctx->offset < ctx->tx_len) {
		size_t chunk = MIN((size_t)SPI_AGM_MAX_TX_BYTES, ctx->tx_len - ctx->offset);
		uint16_t lens[SPI_AGM_MAX_PHASES];
		size_t count = 0U;
		int ret;

		/* The split is a pure function of `chunk` so the native suite can
		 * pin it without a phase engine. */
		count = spi_agm_tx_phase_split(chunk, lens);
		for (size_t i = 0U; i < count; i++) {
			phases[i].lines = SPI_AGM_LINES_SINGLE;
			phases[i].dummy = false;
			phases[i].len = lens[i];
			phases[i].tx = &ctx->tx[ctx->offset + (i * SPI_AGM_MAX_TX_PHASE_BYTES)];
			phases[i].rx = NULL;
		}

		ret = spi_agm_transceive_phases(dev, spi_cfg, phases, count);

		if (ret < 0) {
			return ret;
		}

		ctx->offset += chunk;
	}

	return 0;
}

/* Context for spi_agm_read_long's body callback. */
struct spi_agm_read_long_ctx {
	uint8_t *rx;
	size_t rx_len;
	size_t offset;
};

/* One chunk of the read_long loop: an RX phase long enough to need DMA when
 * chunk > SPI_AGM_PHASE_BYTES. The DUMMY byte clocks the chip-select window
 * open (constraint 1) -- the read data follows from there. */
static int spi_agm_read_long_body(const struct device *dev, const struct spi_config *spi_cfg,
				  void *vctx)
{
	struct spi_agm_read_long_ctx *ctx = vctx;
	struct spi_agm_phase phases[2];

	while (ctx->offset < ctx->rx_len) {
		size_t chunk = MIN((size_t)SPI_AGM_MAX_PHASE_BYTES, ctx->rx_len - ctx->offset);
		size_t phase_count = 0U;

		phases[phase_count].lines = SPI_AGM_LINES_SINGLE;
		phases[phase_count].dummy = true;
		phases[phase_count].len = 1U;
		phases[phase_count].tx = NULL;
		phases[phase_count].rx = NULL;
		phase_count++;

		phases[phase_count].lines = SPI_AGM_LINES_SINGLE;
		phases[phase_count].dummy = false;
		phases[phase_count].len = (uint16_t)chunk;
		phases[phase_count].tx = NULL;
		phases[phase_count].rx = &ctx->rx[ctx->offset];
		phase_count++;

		int ret = spi_agm_transceive_phases(dev, spi_cfg, phases, phase_count);

		if (ret < 0) {
			return ret;
		}

		ctx->offset += chunk;
	}

	return 0;
}

int spi_agm_write_long(const struct device *dev, const struct spi_config *spi_cfg,
		       const uint8_t *tx, size_t tx_len)
{
	struct spi_agm_write_long_ctx ctx;

	if ((tx == NULL) || (tx_len == 0U)) {
		return -EINVAL;
	}

	ctx.tx = tx;
	ctx.tx_len = tx_len;
	ctx.offset = 0U;

	return spi_agm_long_cs_hold(dev, spi_cfg, spi_agm_write_long_body, &ctx);
}

int spi_agm_read_long(const struct device *dev, const struct spi_config *spi_cfg, uint8_t *rx,
		      size_t rx_len)
{
	struct spi_agm_read_long_ctx ctx;

	if ((rx == NULL) || (rx_len == 0U)) {
		return -EINVAL;
	}

	ctx.rx = rx;
	ctx.rx_len = rx_len;
	ctx.offset = 0U;

	return spi_agm_long_cs_hold(dev, spi_cfg, spi_agm_read_long_body, &ctx);
}

static int spi_agm_init(const struct device *dev)
{
	const struct spi_agm_config *config = dev->config;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

#if defined(CONFIG_SPI_ASYNC)
	k_work_init(&((struct spi_agm_data *)dev->data)->async.work, spi_agm_async_finish);
#endif

#if defined(CONFIG_PM_DEVICE)
	int ret;

	ARG_UNUSED(config);

	/* TURN_ON opens the APB gate (soc.c already did at PRE_KERNEL_1) and
	 * applies the pin routing; with runtime PM the device may then suspend
	 * when idle. */
	ret = pm_device_driver_init(dev, spi_agm_pm_action);
	if (ret < 0) {
		return ret;
	}

#if defined(CONFIG_PM_DEVICE_RUNTIME)
	/* Enabling runtime PM suspends an active device, so the gate is closed
	 * until the first transfer asks for it. */
	return pm_device_runtime_enable(dev);
#else
	return 0;
#endif
#else
	return pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
#endif
}

/* `dmas` is optional: without it the register-fed paths (up to 32 bytes out,
 * 4 bytes in) still work and the streaming ones return -ENOTSUP. */
#define SPI_AGM_DMA_INIT(inst)                                                                     \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, dmas),                                             \
		    (.dma = DEVICE_DT_GET(DT_INST_PHANDLE_BY_IDX(inst, dmas, 0)),                  \
		     .dma_tx_channel = DT_INST_PHA_BY_IDX(inst, dmas, 0, channel),                 \
		     .dma_rx_channel = DT_INST_PHA_BY_IDX(inst, dmas, 1, channel),                 \
		     .dma_tx_request = DT_INST_PROP(inst, agm_tx_dma_request),                     \
		     .dma_rx_request = DT_INST_PROP(inst, agm_rx_dma_request),),                   \
		    (.dma = NULL,))

#define SPI_AGM_INIT(inst)                                                                         \
	PINCTRL_DT_INST_DEFINE(inst);                                                              \
	static struct spi_agm_data spi_agm_data_##inst = {                                         \
		.lock = Z_MUTEX_INITIALIZER(spi_agm_data_##inst.lock),                             \
	};                                                                                         \
	PM_DEVICE_DT_INST_DEFINE(inst, spi_agm_pm_action);                                         \
	static const struct spi_agm_config spi_agm_config_##inst = {                               \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(inst)),                                           \
		.clock_freq = DT_INST_PROP_BY_PHANDLE(inst, clocks, clock_frequency),              \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),                                      \
		.apb_clk_bit = DT_INST_PROP(inst, agm_apb_clkenable_bit),                          \
		.multiline_cap = DT_INST_PROP_OR(inst, agm_spi_multiline, 0U),                     \
		SPI_AGM_DMA_INIT(inst)                                                             \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, spi_agm_init, PM_DEVICE_DT_INST_GET(inst),                     \
			      &spi_agm_data_##inst, &spi_agm_config_##inst, POST_KERNEL,          \
			      CONFIG_SPI_INIT_PRIORITY, &spi_agm_api);

DT_INST_FOREACH_STATUS_OKAY(SPI_AGM_INIT)
