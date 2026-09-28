/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Unit tests for spi_agm_tx_phase_split().
 *
 * The property under test is the one the engine enforces and the driver has to
 * agree with: a register-fed TX phase carries at most SPI_AGM_MAX_TX_PHASE_BYTES
 * bytes (the phase data register is 32 bits), and one phase list carries at most
 * SPI_AGM_MAX_PHASES of them. Before the fix the slicing used
 * SPI_AGM_MAX_PHASE_BYTES (4095) per phase, so every spi_agm_write_long() longer
 * than four bytes was rejected by spi_agm_transceive_phases() with -EINVAL --
 * with zero callers in the tree, so no build and no board run said a word.
 */

#include <zephyr/drivers/spi/spi_agm.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

static size_t split(size_t chunk, uint16_t lens[SPI_AGM_MAX_PHASES])
{
	memset(lens, 0, sizeof(uint16_t) * SPI_AGM_MAX_PHASES);
	return spi_agm_tx_phase_split(chunk, lens);
}

/* Every split has to satisfy what the engine accepts, whatever the chunk. */
static void assert_split_is_legal(size_t chunk, size_t count, const uint16_t *lens)
{
	size_t total = 0U;

	zassert_true(count <= SPI_AGM_MAX_PHASES, "chunk %zu: %zu phases is more than the engine takes",
		     chunk, count);

	for (size_t i = 0U; i < count; i++) {
		zassert_true(lens[i] > 0U, "chunk %zu: phase %zu is empty", chunk, i);
		zassert_true(lens[i] <= SPI_AGM_MAX_TX_PHASE_BYTES,
			     "chunk %zu: phase %zu carries %u B, the data register holds %u",
			     chunk, i, lens[i], SPI_AGM_MAX_TX_PHASE_BYTES);
		total += lens[i];
	}

	zassert_equal(total, MIN(chunk, (size_t)SPI_AGM_MAX_PHASES * SPI_AGM_MAX_TX_PHASE_BYTES),
		      "chunk %zu: the phases have to add up to the chunk", chunk);
}

ZTEST(spi_agm_logic, test_00_the_tx_phase_limits_are_the_engine_s)
{
	/* These two numbers are load-bearing in three places (this split, the
	 * write_long loop, and spi_agm_transceive_phases()'s validation), so the
	 * test states them rather than inferring them. */
	zassert_equal(SPI_AGM_MAX_TX_PHASE_BYTES, 4U, "a register-fed TX phase is one 32-bit write");
	zassert_equal(SPI_AGM_MAX_PHASES, 8U, "PHASE_CNT is 3 bits");
}

ZTEST(spi_agm_logic, test_01_small_chunks_are_one_phase_each)
{
	uint16_t lens[SPI_AGM_MAX_PHASES];

	zassert_equal(split(0U, lens), 0U, "a zero-length chunk arms nothing");

	zassert_equal(split(1U, lens), 1U, "one byte is one phase");
	zassert_equal(lens[0], 1U, "carrying one byte");

	zassert_equal(split(4U, lens), 1U, "four bytes fill one phase exactly");
	zassert_equal(lens[0], 4U, "carrying four bytes");
}

ZTEST(spi_agm_logic, test_02_a_chunk_larger_than_one_phase_is_split)
{
	uint16_t lens[SPI_AGM_MAX_PHASES];

	/* The regression this pins: 5 bytes is one full phase plus one byte. The old
	 * slicing produced a single 5-byte phase, which the engine rejects. */
	zassert_equal(split(5U, lens), 2U, "five bytes take two phases");
	zassert_equal(lens[0], 4U, "the first is full");
	zassert_equal(lens[1], 1U, "the second carries the remainder");

	/* 30 bytes: seven full phases and a 2-byte tail. */
	zassert_equal(split(30U, lens), 8U, "thirty bytes take the whole phase list");
	zassert_equal(lens[7], 2U, "with the tail in the last one");
	assert_split_is_legal(30U, 8U, lens);
}

ZTEST(spi_agm_logic, test_03_the_chunk_ceiling_is_eight_phases)
{
	uint16_t lens[SPI_AGM_MAX_PHASES];
	const size_t ceiling = (size_t)SPI_AGM_MAX_PHASES * SPI_AGM_MAX_TX_PHASE_BYTES;

	zassert_equal(split(ceiling, lens), SPI_AGM_MAX_PHASES, "exactly the ceiling takes every phase");
	assert_split_is_legal(ceiling, SPI_AGM_MAX_PHASES, lens);

	/* One byte more is clamped: the write_long loop slices by the ceiling
	 * before calling this, so anything longer is a caller error the split
	 * must not turn into an illegal phase list. */
	zassert_equal(split(ceiling + 1U, lens), SPI_AGM_MAX_PHASES,
		      "chunks beyond the ceiling are clamped, not overflowed");
	assert_split_is_legal(ceiling + 1U, SPI_AGM_MAX_PHASES, lens);

	/* The shape the bug produced, spelled out: a phase may never carry the
	 * 12-bit per-phase maximum when it is register-fed. */
	zassert_equal(split(SPI_AGM_MAX_PHASE_BYTES, lens), SPI_AGM_MAX_PHASES,
		      "the 12-bit phase maximum still has to come out as four-byte phases");
	assert_split_is_legal(SPI_AGM_MAX_PHASE_BYTES, SPI_AGM_MAX_PHASES, lens);
}

ZTEST(spi_agm_logic, test_04_every_chunk_length_satisfies_the_engine)
{
	uint16_t lens[SPI_AGM_MAX_PHASES];

	/* Walk the interesting range so a future edit to the split has to keep
	 * the invariant for all of it, not just for the cases named above. */
	for (size_t chunk = 0U; chunk <= ((size_t)SPI_AGM_MAX_PHASES *
					  SPI_AGM_MAX_TX_PHASE_BYTES) + 2U; chunk++) {
		size_t count = split(chunk, lens);

		assert_split_is_legal(chunk, count, lens);
	}
}

ZTEST_SUITE(spi_agm_logic, NULL, NULL, NULL, NULL, NULL);
