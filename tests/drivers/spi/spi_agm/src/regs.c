/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Register-model suite for the AgRV2K SPI phase engine.
 *
 * Two pieces of test-side machinery:
 *
 *  - the register window is RAM (tests/drivers/common/agm_native);
 *  - the *behaviour* is a 1 ms timer that plays the engine: when it sees CTRL
 *    .START it clears it and raises CTRL.DONE (optionally with CTRL.ERROR).
 *    Without that the driver's wait_done() would spin to its ~150 ms timeout on
 *    every transfer and spi_agm_collect() would never run -- and collect() is
 *    where the half-duplex RX semantics live, which is the part of this driver
 *    a caller has to understand (upstream spi-nor depends on it).
 *
 * What that covers: the phase programming (TX phases, the RX phase, the
 * CS-window shape), the RX slot semantics (bytes the command occupied come back
 * as 0xFF, the captured stream lands at the SPI API's offsets), the -ENOTSUP
 * gates (RX-first, RX wholly inside the TX window, a long RX without DMA on the
 * node, multi-line without the bitstream capability), and the engine's own
 * error/timeout paths.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/spi/spi_agm.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <string.h>

#define SPI_DEV  DEVICE_DT_GET(DT_NODELABEL(spi_agm0))

/* The bus shape the driver claims: controller, 8-bit words, MSB first. The
 * word size is part of `operation`, not a default -- a bare
 * SPI_OP_MODE_CONTROLLER has SPI_WORD_SIZE_GET() == 0 and every transfer is
 * refused with -ENOTSUP before it reaches the engine. */
#define CFG_8BIT  (SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB)

/* Register offsets (drivers/spi/spi_agm.c). */
#define R_CTRL        0x00U
#define R_PHASE_CTRL(n) (0x10U + (4U * (n)))
#define R_PHASE_DATA(n) (0x30U + (4U * (n)))

#define CTRL_START      BIT(0)
#define CTRL_DONE       BIT(1)
#define CTRL_ERROR      BIT(2)
#define CTRL_PHASE_CNT_SHIFT 4U
#define CTRL_DMA_EN     BIT(8)
#define CTRL_SCLK_DIV_SHIFT 12U

#define PHASE_ACTION_SHIFT 4U
#define PHASE_ACTION_TX    (0U << PHASE_ACTION_SHIFT)
#define PHASE_ACTION_RX    (2U << PHASE_ACTION_SHIFT)
#define PHASE_BYTE_CNT_SHIFT 8U

#define SPI_BASE 0x40012000UL

static uint32_t rd(uint32_t off)
{
	return *(volatile uint32_t *)(SPI_BASE + off);
}

static void wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(SPI_BASE + off) = val;
}

/* ---- the engine model ------------------------------------------------- */

static struct k_timer engine_timer;
static bool engine_error;
static volatile uint32_t engine_ticks;

static void engine_expire(struct k_timer *timer)
{
	uint32_t ctrl = rd(R_CTRL);

	ARG_UNUSED(timer);

	engine_ticks++;
	if ((ctrl & CTRL_START) == 0U) {
		return;
	}

	/* One START = one phase list. Complete it at once (the real engine
	 * takes a few microseconds) and report an error if the case asked. */
	ctrl &= ~CTRL_START;
	ctrl |= CTRL_DONE;
	if (engine_error) {
		ctrl |= CTRL_ERROR;
	}
	wr(R_CTRL, ctrl);
}

/* ZTEST_SUITE's `setup` slot takes no argument and returns the fixture (there
 * is none here); `before` is the per-case hook that takes the fixture. */
static void *suite_setup(void)
{
	engine_error = false;
	k_timer_init(&engine_timer, engine_expire, NULL);
	k_timer_start(&engine_timer, K_MSEC(1), K_MSEC(1));

	return NULL;
}

static void suite_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	k_timer_stop(&engine_timer);
}

static void case_setup(void *fixture)
{
	ARG_UNUSED(fixture);

	engine_error = false;
	wr(R_CTRL, 0U);
	for (uint32_t i = 0U; i < SPI_AGM_MAX_PHASES; i++) {
		wr(R_PHASE_CTRL(i), 0U);
		wr(R_PHASE_DATA(i), 0U);
	}
}

ZTEST_SUITE(spi_agm_regs, NULL, suite_setup, case_setup, NULL, suite_teardown);

/* The model has to be alive *during* the driver's poll: spi_agm_wait_done()
 * busy-waits on CTRL.DONE, and native_sim only advances simulated time inside
 * k_busy_wait() (arch_busy_wait() programs a wake-up and halts the CPU), so a
 * timer that expires while the driver spins is what completes a transfer. This
 * case pins that assumption: if it stops holding, every positive case below
 * turns into -ETIMEDOUT and this says why first. */
ZTEST(spi_agm_regs, test_00_the_engine_model_runs_during_a_busy_wait)
{
	uint32_t before = engine_ticks;

	k_busy_wait(20000U);	/* 20 ms of simulated time */

	zassert_true(engine_ticks > before,
		     "the engine timer has to expire while the CPU busy-waits");
}

/* ---- the simple shapes ------------------------------------------------ */

ZTEST(spi_agm_regs, test_01_one_tx_phase_carries_the_bytes)
{
	uint8_t tx[4] = { 0x9fU, 0x00U, 0x00U, 0x00U };
	struct spi_config cfg = { .frequency = 1000000U, .operation = CFG_8BIT };
	struct spi_buf buf = { .buf = tx, .len = sizeof(tx) };
	struct spi_buf_set tx_set = { .buffers = &buf, .count = 1U };

	zassert_true(device_is_ready(SPI_DEV), "the SPI device came up against the fake window");
	zassert_ok(spi_transceive(SPI_DEV, &cfg, &tx_set, NULL), "a TX-only transfer runs");

	zassert_equal((rd(R_PHASE_CTRL(0)) >> PHASE_ACTION_SHIFT) & 0x7U,
		      PHASE_ACTION_TX >> PHASE_ACTION_SHIFT, "phase 0 is a TX phase");
	zassert_equal((rd(R_PHASE_CTRL(0)) >> PHASE_BYTE_CNT_SHIFT) & 0xfffU, 4U,
		      "carrying all four bytes the data register holds");
	/* The words are packed little-endian, so byte 0 is clocked first. */
	zassert_equal(rd(R_PHASE_DATA(0)), 0x0000009fU, "PHASE_DATA holds the packed bytes");
}

ZTEST(spi_agm_regs, test_02_a_long_tx_is_split_over_several_phases)
{
	uint8_t tx[10];
	struct spi_config cfg = { .frequency = 1000000U, .operation = CFG_8BIT };
	struct spi_buf buf = { .buf = tx, .len = sizeof(tx) };
	struct spi_buf_set tx_set = { .buffers = &buf, .count = 1U };

	for (size_t i = 0U; i < sizeof(tx); i++) {
		tx[i] = (uint8_t)i;
	}

	zassert_ok(spi_transceive(SPI_DEV, &cfg, &tx_set, NULL));

	/* Three phases: 4 + 4 + 2 bytes -- the engine's data register is the
	 * only source for a register-fed TX phase. */
	zassert_equal((rd(R_PHASE_CTRL(0)) >> PHASE_BYTE_CNT_SHIFT) & 0xfffU, 4U, "phase 0: 4 bytes");
	zassert_equal((rd(R_PHASE_CTRL(1)) >> PHASE_BYTE_CNT_SHIFT) & 0xfffU, 4U, "phase 1: 4 bytes");
	zassert_equal((rd(R_PHASE_CTRL(2)) >> PHASE_BYTE_CNT_SHIFT) & 0xfffU, 2U, "phase 2: the tail");
	zassert_equal(rd(R_PHASE_DATA(2)), 0x00000908U, "and the tail's bytes are packed there");
}

ZTEST(spi_agm_regs, test_03_the_clock_divider_comes_from_the_requested_frequency)
{
	uint8_t tx[1] = { 0U };
	struct spi_config cfg = { .frequency = 1000000U, .operation = CFG_8BIT };
	struct spi_buf buf = { .buf = tx, .len = sizeof(tx) };
	struct spi_buf_set tx_set = { .buffers = &buf, .count = 1U };

	zassert_ok(spi_transceive(SPI_DEV, &cfg, &tx_set, NULL));

	/* 200 MHz / div <= 1 MHz first holds at the divider 256, which is the
	 * slowest the engine has -- and the field stores it as 0 ("0 meaning
	 * 256", SPI_AGM_DIV_MAX's encoding). */
	uint32_t div = (rd(R_CTRL) >> CTRL_SCLK_DIV_SHIFT) & 0xffU;

	zassert_equal(div, 0U,
		      "1 MHz at 200 MHz needs the 256 divider, encoded as 0 in the field");
}

/* ---- the half-duplex RX semantics ------------------------------------- */

ZTEST(spi_agm_regs, test_10_rx_slots_inside_the_tx_window_come_back_as_ff)
{
	uint8_t tx[4] = { 0x03U, 0x00U, 0x00U, 0x00U };
	uint8_t rx[6] = { 0xa5U, 0xa5U, 0xa5U, 0xa5U, 0xa5U, 0xa5U };
	struct spi_config cfg = { .frequency = 1000000U, .operation = CFG_8BIT };
	struct spi_buf txb = { .buf = tx, .len = sizeof(tx) };
	struct spi_buf rxb = { .buf = rx, .len = sizeof(rx) };
	struct spi_buf_set tx_set = { .buffers = &txb, .count = 1U };
	struct spi_buf_set rx_set = { .buffers = &rxb, .count = 1U };

	/* Wait, the way a flash driver does: the command's slots are in the RX
	 * list too, and only the two slots after them carry data. */
	wr(R_PHASE_DATA(1), 0x0000beefU);
	zassert_ok(spi_transceive(SPI_DEV, &cfg, &tx_set, &rx_set));

	/* The engine sampled nothing while it clocked the command out. */
	zassert_equal(rx[0], 0xffU, "the command's slot is filler");
	zassert_equal(rx[3], 0xffU, "and so are the rest of the TX window's slots");
	zassert_equal(rx[4], 0xefU, "the captured stream starts after the TX window");
	zassert_equal(rx[5], 0xbeU, "byte by byte, little endian");

	/* The RX phase carries the slots the TX phases did not clock. */
	zassert_equal((rd(R_PHASE_CTRL(1)) >> PHASE_ACTION_SHIFT) & 0x7U,
		      PHASE_ACTION_RX >> PHASE_ACTION_SHIFT, "phase 1 is the RX phase");
	zassert_equal((rd(R_PHASE_CTRL(1)) >> PHASE_BYTE_CNT_SHIFT) & 0xfffU, 2U,
		      "and it carries the two slots the TX window left over");
}

ZTEST(spi_agm_regs, test_11_rx_entirely_inside_the_tx_window_is_refused)
{
	uint8_t tx[4] = { 0x03U, 0U, 0U, 0U };
	uint8_t rx[2] = { 0U };
	struct spi_config cfg = { .frequency = 1000000U, .operation = CFG_8BIT };
	struct spi_buf txb = { .buf = tx, .len = 4U };
	struct spi_buf rxb = { .buf = rx, .len = 2U };
	struct spi_buf_set tx_set = { .buffers = &txb, .count = 1U };
	struct spi_buf_set rx_set = { .buffers = &rxb, .count = 1U };

	/* Every requested RX byte sits in the command's window: there is no
	 * phase to run for them, and returning filler would look like data. */
	zassert_equal(spi_transceive(SPI_DEV, &cfg, &tx_set, &rx_set), -ENOTSUP,
		      "the engine cannot sample MISO while it clocks the command");
}

ZTEST(spi_agm_regs, test_12_the_engine_cannot_start_with_rx)
{
	uint8_t rx[2] = { 0U };
	struct spi_config cfg = { .frequency = 1000000U, .operation = CFG_8BIT };
	struct spi_buf rxb = { .buf = rx, .len = 2U };
	struct spi_buf_set rx_set = { .buffers = &rxb, .count = 1U };

	zassert_equal(spi_transceive(SPI_DEV, &cfg, NULL, &rx_set), -ENOTSUP,
		      "the first phase has to put something on MOSI");
}

/* ---- the gates the engine cannot express ------------------------------ */

ZTEST(spi_agm_regs, test_20_dma_only_shapes_are_refused_without_a_dmas_property)
{
	uint8_t tx[4] = { 0U };
	uint8_t rx[32] = { 0U };
	struct spi_config cfg = { .frequency = 1000000U, .operation = CFG_8BIT };
	struct spi_buf txb = { .buf = tx, .len = sizeof(tx) };
	struct spi_buf rxb = { .buf = rx, .len = sizeof(rx) };
	struct spi_buf_set tx_set = { .buffers = &txb, .count = 1U };
	struct spi_buf_set rx_set = { .buffers = &rxb, .count = 1U };

	/* More than the data register can hold needs the DMAC; this node has no
	 * `dmas`, which is the documented -ENOTSUP rather than a silent 4-byte
	 * transfer. */
	zassert_equal(spi_transceive(SPI_DEV, &cfg, &tx_set, &rx_set), -ENOTSUP,
		      "a long RX needs DMA and this node does not declare any");
}

ZTEST(spi_agm_regs, test_21_the_engine_s_own_limits_are_refused)
{
	uint8_t tx[1] = { 0U };
	struct spi_config cfg = { .frequency = 1000000U, .operation = CFG_8BIT };
	struct spi_buf txb = { .buf = tx, .len = sizeof(tx) };
	struct spi_buf_set tx_set = { .buffers = &txb, .count = 1U };

	/* Mode 0 only: CPOL/CPHA live in the fabric patch, not in this IP. */
	cfg.operation = CFG_8BIT | SPI_MODE_CPHA;
	zassert_equal(spi_transceive(SPI_DEV, &cfg, &tx_set, NULL), -ENOTSUP,
		      "CPHA != 0 is a fabric capability");

	/* MSB-first comes out of the data register's packing. */
	cfg.operation = CFG_8BIT | SPI_TRANSFER_LSB;
	zassert_equal(spi_transceive(SPI_DEV, &cfg, &tx_set, NULL), -ENOTSUP,
		      "LSB-first is refused rather than silently MSB");

	/* The CS window is one phase run, so there is nothing to hold across
	 * calls. */
	cfg.operation = CFG_8BIT | SPI_HOLD_ON_CS;
	zassert_equal(spi_transceive(SPI_DEV, &cfg, &tx_set, NULL), -ENOTSUP, "HOLD_ON_CS");

	/* A controller-driven CS cannot be active high (the engine's CSN is). */
	cfg.operation = CFG_8BIT | SPI_CS_ACTIVE_HIGH;
	zassert_equal(spi_transceive(SPI_DEV, &cfg, &tx_set, NULL), -ENOTSUP, "active-high CS");
}

ZTEST(spi_agm_regs, test_22_an_engine_error_is_reported)
{
	uint8_t tx[1] = { 0x5aU };
	struct spi_config cfg = { .frequency = 1000000U, .operation = CFG_8BIT };
	struct spi_buf txb = { .buf = tx, .len = sizeof(tx) };
	struct spi_buf_set tx_set = { .buffers = &txb, .count = 1U };

	engine_error = true;
	zassert_equal(spi_transceive(SPI_DEV, &cfg, &tx_set, NULL), -EIO,
		      "CTRL.ERROR becomes -EIO, not a silent success");
	engine_error = false;

	/* And the driver soft-resets the engine after a failure, so the next
	 * transfer starts from a known state instead of mid-frame. */
	zassert_equal(rd(R_CTRL) & (CTRL_START | CTRL_DONE), 0U,
		      "the engine was left idle rather than mid-transfer");
}

/* ---- the extension entry points --------------------------------------- */

ZTEST(spi_agm_regs, test_30_clock_dummy_programs_a_filler_phase)
{
	struct spi_config cfg = { .frequency = 1000000U, .operation = CFG_8BIT };

	zassert_ok(spi_agm_clock_dummy(SPI_DEV, &cfg, 3U), "clock three filler bytes");

	/* DUMMY is action 1, with no data register involvement. */
	zassert_equal((rd(R_PHASE_CTRL(0)) >> PHASE_ACTION_SHIFT) & 0x7U, 1U, "a DUMMY phase");
	zassert_equal((rd(R_PHASE_CTRL(0)) >> PHASE_BYTE_CNT_SHIFT) & 0xfffU, 3U, "three bytes");

	zassert_equal(spi_agm_clock_dummy(SPI_DEV, &cfg, 0U), -EINVAL, "zero filler is refused");
}

ZTEST(spi_agm_regs, test_31_poll_status_programs_the_command_and_the_poll_phase)
{
	static const uint8_t rdsr = 0x05U;
	struct spi_config cfg = { .frequency = 1000000U, .operation = CFG_8BIT };

	/* The engine's POLL phase compares the byte a device answers with
	 * `expect` under `mask`, up to `limit` times, all inside one CS window:
	 * the flash "wait for WIP" loop in hardware. The model never reports a
	 * phase error, so this is the success path. */
	zassert_ok(spi_agm_poll_status(SPI_DEV, &cfg, &rdsr, 1U, 0x01U, 0x00U, 100U),
		   "polling for WEL to clear");

	zassert_equal((rd(R_PHASE_CTRL(0)) >> PHASE_ACTION_SHIFT) & 0x7U,
		      PHASE_ACTION_TX >> PHASE_ACTION_SHIFT, "phase 0 sends the command");
	zassert_equal(rd(R_PHASE_DATA(0)), rdsr, "the command byte");
	zassert_equal((rd(R_PHASE_CTRL(1)) >> PHASE_ACTION_SHIFT) & 0x7U, 3U,
		      "phase 1 is the POLL phase");
	/* limit | mask | expect, from the top byte down. */
	zassert_equal(rd(R_PHASE_DATA(1)), (100U << 24) | (0x01U << 16) | (0x00U << 8),
		      "the poll settings live in the phase's data register");

	zassert_equal(spi_agm_poll_status(SPI_DEV, &cfg, NULL, 1U, 0U, 0U, 1U), -EINVAL,
		      "a poll without a command is refused");
}

ZTEST(spi_agm_regs, test_32_a_phase_list_is_validated)
{
	struct spi_config cfg = { .frequency = 1000000U, .operation = CFG_8BIT };
	uint8_t byte = 0U;
	struct spi_agm_phase ok = {
		.lines = SPI_AGM_LINES_SINGLE, .dummy = false, .len = 1U, .tx = &byte, .rx = NULL,
	};
	struct spi_agm_phase bad = ok;

	zassert_ok(spi_agm_transceive_phases(SPI_DEV, &cfg, &ok, 1U), "one TX phase is legal");

	/* A register-fed TX phase cannot carry more than the data register. */
	bad.len = SPI_AGM_MAX_TX_PHASE_BYTES + 1U;
	zassert_equal(spi_agm_transceive_phases(SPI_DEV, &cfg, &bad, 1U), -EINVAL,
		      "five bytes in one TX phase is not expressible");

	/* Zero-length phases, too many phases, and RX that is not last are
	 * caller errors. */
	bad = ok;
	bad.len = 0U;
	zassert_equal(spi_agm_transceive_phases(SPI_DEV, &cfg, &bad, 1U), -EINVAL, "empty phase");

	zassert_equal(spi_agm_transceive_phases(SPI_DEV, &cfg, &ok,
						SPI_AGM_MAX_PHASES + 1U), -EINVAL,
		      "more phases than the engine has");
}
