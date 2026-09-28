/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief native_sim suite for the AgRV2K DMAC driver.
 *
 * The driver's contract is memory-shaped: it flattens the caller's block list
 * into an LLI chain in SRAM and programs the channel registers from the head of
 * that chain. With the register window backed by RAM
 * (tests/drivers/common/agm_native) both halves are readable, so this suite
 * walks the LLI chain the driver built and checks the channel programming
 * field by field. Nothing here needs the DMA engine to *run* -- completion is
 * the interrupt's job and there is no hardware on native_sim, which is why the
 * cases assert on what was programmed rather than on a callback.
 *
 * It pins two contracts:
 *  - every entry point that takes a channel number bounds-checks it
 *    (agm_dma_get_chan() indexes data->chan[] with no bound of its own);
 *  - a transfer whose blocks are all zero-length is refused, instead of
 *    programming the channel from a *stale* lli[0] and letting start() enable
 *    it -- i.e. replaying the previous transfer.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/ztest.h>

#include <string.h>

#define DMA_DEV  DEVICE_DT_GET(DT_NODELABEL(dma_agm0))

/* Global register offsets (drivers/dma/dma_agm.c). */
#define DMA_INTTCCLEAR    0x008U
#define DMA_INTERRCLEAR   0x010U
#define DMA_ENBLDCHNS     0x01cU
#define DMA_CONFIGURATION 0x030U
#define DMA_SYNC          0x034U

/* Per-channel offsets from the channel base. */
#define DMA_CHAN_BASE   0x100U
#define DMA_CHAN_STRIDE 0x20U
#define CH_SRCADDR 0x00U
#define CH_DSTADDR 0x04U
#define CH_LLI     0x08U
#define CH_CONTROL 0x0cU
#define CH_CONFIG  0x10U

#define DMA_BASE 0x41000000UL

#define CFG_E         BIT(0)
#define CFG_SRC_PERIPH_OFF 1U
#define CFG_DST_PERIPH_OFF 6U
#define CFG_FLOW_OFF       11U
#define CFG_ERR_INT  BIT(14)
#define CFG_TC_INT   BIT(15)

#define CTRL_SIZE_MASK 0xfffU
#define CTRL_SWIDTH_OFF 18U
#define CTRL_DWIDTH_OFF 21U
#define CTRL_SRC_INCR BIT(26)
#define CTRL_DST_INCR BIT(27)
#define CTRL_TC_INT   BIT(31)

/* The driver's own LLI layout; the test interprets the chain with it. */
struct lli {
	uint32_t src;
	uint32_t dst;
	uint32_t lli;
	uint32_t control;
};

static uint32_t rd(uint32_t off)
{
	return *(volatile uint32_t *)(DMA_BASE + off);
}

static void wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(DMA_BASE + off) = val;
}

static uint32_t ch_off(uint32_t ch, uint32_t reg)
{
	return DMA_CHAN_BASE + (ch * DMA_CHAN_STRIDE) + reg;
}

static void suite_reset_fake_regs(void *fixture)
{
	ARG_UNUSED(fixture);

	/* The driver's init leaves CONFIGURATION = E; clear the per-channel
	 * state each case programs, plus the sync/status registers. */
	wr(DMA_SYNC, 0U);
	wr(DMA_ENBLDCHNS, 0U);
	for (uint32_t ch = 0U; ch < 8U; ch++) {
		wr(ch_off(ch, CH_CONFIG), 0U);
		wr(ch_off(ch, CH_SRCADDR), 0U);
		wr(ch_off(ch, CH_DSTADDR), 0U);
		wr(ch_off(ch, CH_LLI), 0U);
		wr(ch_off(ch, CH_CONTROL), 0U);
	}
}

ZTEST_SUITE(dma_agm, NULL, NULL, suite_reset_fake_regs, NULL, NULL);

/* ---- init ------------------------------------------------------------ */

ZTEST(dma_agm, test_01_init_enables_the_controller_and_clears_the_channels)
{
	zassert_true(device_is_ready(DMA_DEV), "the DMAC came up against the fake window");

	zassert_equal(rd(DMA_CONFIGURATION) & CFG_E, CFG_E, "the controller is enabled");
	for (uint32_t ch = 0U; ch < 8U; ch++) {
		zassert_equal(rd(ch_off(ch, CH_CONFIG)), 0U, "channel %u starts disabled", ch);
	}
}

/* ---- the channel number is checked on every entry point --------------- */

ZTEST(dma_agm, test_10_out_of_range_channel_is_refused_everywhere)
{
	struct dma_config cfg = { 0 };
	struct dma_block_config block = { 0 };
	struct dma_status status;

	zassert_equal(dma_start(DMA_DEV, 8U), -EINVAL, "start: channel 8 does not exist");
	zassert_equal(dma_start(DMA_DEV, UINT32_MAX), -EINVAL, "start: nor UINT32_MAX");
	zassert_equal(dma_stop(DMA_DEV, 8U), -EINVAL, "stop: channel 8 does not exist");
	zassert_equal(dma_get_status(DMA_DEV, 8U, &status), -EINVAL,
		      "get_status: channel 8 does not exist");

	block.block_size = 4U;
	cfg.head_block = &block;
	cfg.block_count = 1U;
	cfg.source_data_size = 4U;
	cfg.dest_data_size = 4U;
	cfg.channel_direction = MEMORY_TO_MEMORY;
	zassert_equal(dma_config(DMA_DEV, 8U, &cfg), -EINVAL, "config: channel 8 does not exist");

	/* A NULL status pointer is a caller error, not a crash: the driver used
	 * to memset() straight through it. */
	zassert_equal(dma_get_status(DMA_DEV, 0U, NULL), -EINVAL, "get_status: NULL status");
}

/* ---- a zero-length transfer must not enable the channel ---------------- */

ZTEST(dma_agm, test_20_zero_sized_blocks_are_refused_not_replayed)
{
	struct dma_block_config block = { 0 };
	struct dma_config cfg = { 0 };
	uint8_t src[8] = { 0 };

	/* A real transfer first, so lli[0] holds something a careless driver
	 * could point the channel at. */
	block.source_address = (uint32_t)(uintptr_t)src;
	block.dest_address = (uint32_t)(uintptr_t)src;
	block.block_size = 8U;
	block.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	block.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	cfg.head_block = &block;
	cfg.block_count = 1U;
	cfg.source_data_size = 4U;
	cfg.dest_data_size = 4U;
	cfg.channel_direction = MEMORY_TO_MEMORY;
	zassert_ok(dma_config(DMA_DEV, 0U, &cfg), "a normal transfer configures");
	zassert_ok(dma_start(DMA_DEV, 0U), "and starts");
	zassert_ok(dma_stop(DMA_DEV, 0U), "and stops");

	/* Now the zero-length one: every block asks for zero bytes. */
	block.block_size = 0U;
	zassert_ok(dma_config(DMA_DEV, 0U, &cfg), "config does not validate the sizes");
	zassert_equal(dma_start(DMA_DEV, 0U), -EINVAL,
		      "a zero-length transfer is refused, not programmed from the stale LLI");
	zassert_equal(rd(ch_off(0U, CH_CONFIG)) & CFG_E, 0U,
		      "and the channel is left disabled");
}

/* ---- the programming itself ------------------------------------------ */

ZTEST(dma_agm, test_30_two_blocks_become_a_linked_chain)
{
	static uint8_t src0[16];
	static uint8_t src1[16];
	struct dma_block_config b1 = { 0 };
	struct dma_block_config b0 = { 0 };
	struct dma_config cfg = { 0 };
	void *lli0;
	struct lli item;

	b0.source_address = (uint32_t)(uintptr_t)src0;
	b0.dest_address = (uint32_t)(uintptr_t)src1;
	b0.block_size = 16U;
	b0.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	b0.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	b0.next_block = &b1;

	b1.source_address = (uint32_t)(uintptr_t)(src0 + 16U);
	b1.dest_address = (uint32_t)(uintptr_t)(src1 + 16U);
	b1.block_size = 8U;
	b1.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	b1.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;

	cfg.head_block = &b0;
	cfg.block_count = 2U;
	cfg.source_data_size = 4U;
	cfg.dest_data_size = 4U;
	cfg.channel_direction = MEMORY_TO_MEMORY;

	zassert_ok(dma_config(DMA_DEV, 3U, &cfg));
	zassert_ok(dma_start(DMA_DEV, 3U));

	/* The channel is programmed from the first LLI. */
	zassert_equal(rd(ch_off(3U, CH_SRCADDR)), b0.source_address, "source from the first block");
	zassert_equal(rd(ch_off(3U, CH_DSTADDR)), b0.dest_address, "destination from the first block");
	zassert_equal(rd(ch_off(3U, CH_CONFIG)) & CFG_E, CFG_E, "the channel is enabled");
	zassert_equal(rd(ch_off(3U, CH_CONFIG)) & (CFG_TC_INT | CFG_ERR_INT),
		      CFG_TC_INT | CFG_ERR_INT, "both interrupts are armed");
	zassert_equal((rd(ch_off(3U, CH_CONFIG)) >> CFG_FLOW_OFF) & 0x7U, 0U,
		      "memory-to-memory uses flow 0");

	/* The first item's control word: 16 B is 4 units of 4 B, both
	 * addresses increment, and the terminal count belongs to the *last*
	 * item only. */
	zassert_equal(rd(ch_off(3U, CH_CONTROL)) & CTRL_SIZE_MASK, 4U, "4 units in the first item");
	zassert_equal((rd(ch_off(3U, CH_CONTROL)) >> CTRL_SWIDTH_OFF) & 0x7U, 2U,
		      "source width code for 4 bytes");
	zassert_equal((rd(ch_off(3U, CH_CONTROL)) >> CTRL_DWIDTH_OFF) & 0x7U, 2U,
		      "destination width code for 4 bytes");
	zassert_equal(rd(ch_off(3U, CH_CONTROL)) & (CTRL_SRC_INCR | CTRL_DST_INCR),
		      CTRL_SRC_INCR | CTRL_DST_INCR, "both addresses increment");
	zassert_equal(rd(ch_off(3U, CH_CONTROL)) & CTRL_TC_INT, 0U,
		      "the first item does not raise the terminal count");

	/* ... and the chain the driver built in SRAM. */
	lli0 = (void *)(uintptr_t)rd(ch_off(3U, CH_LLI));
	zassert_not_null(lli0, "the first item is linked to the second");
	memcpy(&item, lli0, sizeof(item));
	zassert_equal(item.src, b1.source_address, "second item: source");
	zassert_equal(item.dst, b1.dest_address, "second item: destination");
	zassert_equal(item.lli, 0U, "second item terminates the chain");
	zassert_equal(item.control & CTRL_SIZE_MASK, 2U, "second item: 8 B is 2 units");
	zassert_equal(item.control & CTRL_TC_INT, CTRL_TC_INT,
		      "the terminal count is on the last item");

	zassert_ok(dma_stop(DMA_DEV, 3U));
	zassert_equal(rd(ch_off(3U, CH_CONFIG)), 0U, "stop disables the channel");
}

ZTEST(dma_agm, test_31_peripheral_flow_sets_the_request_line_and_keeps_src_put)
{
	static uint8_t src[4];
	struct dma_block_config block = { 0 };
	struct dma_config cfg = { 0 };

	block.source_address = (uint32_t)(uintptr_t)src;
	block.dest_address = 0x40012000U;	/* SPI0's data register */
	block.block_size = 4U;
	block.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	block.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;

	cfg.head_block = &block;
	cfg.block_count = 1U;
	cfg.source_data_size = 4U;
	cfg.dest_data_size = 4U;
	cfg.channel_direction = MEMORY_TO_PERIPHERAL;
	cfg.dma_slot = 6U;	/* SPI0 TX request line */

	zassert_ok(dma_config(DMA_DEV, 2U, &cfg));
	zassert_ok(dma_start(DMA_DEV, 2U));

	zassert_equal((rd(ch_off(2U, CH_CONFIG)) >> CFG_FLOW_OFF) & 0x7U, 5U,
		      "memory-to-peripheral uses the peripheral-controlled flow (5)");
	zassert_equal((rd(ch_off(2U, CH_CONFIG)) >> CFG_DST_PERIPH_OFF) & 0x1fU, 6U,
		      "the DMA request line comes from the client's dma_slot");
	zassert_equal(rd(ch_off(2U, CH_CONTROL)) & CTRL_DST_INCR, 0U,
		      "a fixed destination does not increment");
	zassert_equal(rd(DMA_SYNC) & BIT(6), BIT(6),
		      "the request line's synchroniser is disabled (single-cycle pulses)");
}

ZTEST(dma_agm, test_32_bad_configurations_are_refused)
{
	struct dma_block_config block = { 0 };
	struct dma_config cfg = { 0 };
	static uint8_t src[8];

	block.source_address = (uint32_t)(uintptr_t)src;
	block.dest_address = (uint32_t)(uintptr_t)src;
	block.block_size = 8U;
	block.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	block.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	cfg.head_block = &block;
	cfg.block_count = 1U;
	cfg.source_data_size = 4U;
	cfg.dest_data_size = 4U;
	cfg.channel_direction = MEMORY_TO_MEMORY;

	zassert_equal(dma_config(DMA_DEV, 1U, NULL), -EINVAL, "no config at all");

	cfg.head_block = NULL;
	zassert_equal(dma_config(DMA_DEV, 1U, &cfg), -EINVAL, "no block list");
	cfg.head_block = &block;

	cfg.block_count = 0U;
	zassert_equal(dma_config(DMA_DEV, 1U, &cfg), -EINVAL, "no blocks");
	cfg.block_count = 1U;

	cfg.dest_data_size = 2U;
	zassert_equal(dma_config(DMA_DEV, 1U, &cfg), -EINVAL, "mixed widths are not supported");
	cfg.dest_data_size = 4U;

	cfg.source_data_size = 3U;
	zassert_equal(dma_config(DMA_DEV, 1U, &cfg), -EINVAL, "an 8/16/32-bit width only");
	cfg.source_data_size = 4U;

	/* The field is 3 bits wide, so pick an out-of-range *fittable* value
	 * (the four directions are 0..3). */
	cfg.channel_direction = (enum dma_channel_direction)7;
	zassert_equal(dma_config(DMA_DEV, 1U, &cfg), -EINVAL, "an unknown direction");
	cfg.channel_direction = MEMORY_TO_MEMORY;

	/* The block-list checks live in program(), i.e. at start() time. */
	block.source_addr_adj = DMA_ADDR_ADJ_DECREMENT;
	zassert_ok(dma_config(DMA_DEV, 1U, &cfg), "config accepts the list");
	zassert_equal(dma_start(DMA_DEV, 1U), -EINVAL, "a decrementing address is refused");
	block.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;

	block.block_size = 6U;	/* not a multiple of the 4-byte width */
	zassert_ok(dma_config(DMA_DEV, 1U, &cfg));
	zassert_equal(dma_start(DMA_DEV, 1U), -EINVAL, "a partial unit is refused");

	zassert_equal(rd(ch_off(1U, CH_CONFIG)) & CFG_E, 0U,
		      "none of the refused starts enabled the channel");
}
