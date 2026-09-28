/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Compressed-bitstream decoder: does it reproduce the config?
 *
 * `CONFIG_AGM_FCB_BITSTREAM_COMPRESSED` changes what soc.c streams into the
 * FCB, and getting it wrong is the kind of mistake that costs a board (the
 * fabric never comes up and the CPU's clock goes with it -- recovery needs
 * BOOT0). So the decoder is checked here, off-target, against a fixture built
 * from the board's canonical bitstream: the same 99944 bytes the uncompressed
 * path streams, compressed by tools/compress_bitstream.py.
 *
 * The fixture is only the compressed stream (9685 B); the expected output is
 * pinned by its CRC32 instead of being carried around as 100 KB. That CRC is
 * the same number the bitstream record uses on the dev board for this file
 * (0x361d90d4), so a change that
 * broke the decoder would fail here *and* be recognisable in a boot log.
 */

#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include <string.h>

#include "fcb_lzw.h"

/* One Supra config: FCB_AUTO_WORDS = 24986 words = 99944 bytes, of which the
 * first two words are stored raw in front of the LZW stream. */
#define CONFIG_WORDS 24986U
#define CONFIG_BYTES (CONFIG_WORDS * 4U)
/* Not CONFIG_CRC: that is the Kconfig symbol for Zephyr's CRC library. */
#define EXPECTED_CRC 0x361d90d4U
/* Upper bound for a compressed config, so `tampered` below can be a plain
 * array (the fixture itself is only known as an extern + length). */
#define FIXTURE_MAX  16384U

extern const unsigned char agm_compressed_bitstream[];
extern const size_t agm_compressed_bitstream_len;

static uint8_t decoded[CONFIG_BYTES];
/* Words written so far, including the two raw header words: the real caller
 * writes IDCODE/USERID before it starts decoding (fcb.c does the same), and
 * the decoder's words follow them. So this starts at 2, not 0. */
static uint32_t emitted;

/* The real caller hands each word to FCB->AUTO; here they land in a buffer in
 * the same little-endian order, which is what the raw path streams. */
static void collect(void *ctx, uint32_t word)
{
	uint32_t off = emitted * 4U;

	ARG_UNUSED(ctx);
	zassert_true(off + 4U <= sizeof(decoded), "decoder emitted too much");
	decoded[off + 0U] = (uint8_t)word;
	decoded[off + 1U] = (uint8_t)(word >> 8);
	decoded[off + 2U] = (uint8_t)(word >> 16);
	decoded[off + 3U] = (uint8_t)(word >> 24);
	emitted++;
}

/* The decoder is told how many words to produce and never reads a length from
 * the stream, so "how much did it emit" is part of the contract. */
static int decode_fixture(void)
{
	memset(decoded, 0, sizeof(decoded));
	/* The fixture's first 8 bytes are IDCODE/USERID, written raw by the
	 * caller in the real path -- copy them the same way. */
	memcpy(decoded, agm_compressed_bitstream, 8U);
	emitted = 2U;
	return fcb_lzw_decode(agm_compressed_bitstream + 8U, CONFIG_WORDS - 2U,
			      collect, NULL);
}

ZTEST(fcb_lzw, test_reproduces_the_canonical_bitstream)
{
	zassert_ok(decode_fixture(), "the fixture has to decode");
	zassert_equal(emitted, CONFIG_WORDS, "word count (header included)");
	zassert_mem_equal(decoded, agm_compressed_bitstream, 8U,
			  "the two header words stay raw");
	zassert_equal(crc32_ieee(decoded, sizeof(decoded)), EXPECTED_CRC,
		      "decoded bytes have to be the bitstream the board runs");
}

/* A stream that ends mid-word is what a truncated upload looks like; the
 * decoder has no length to check against, so what it must not do is claim
 * success on a config it did not produce. Here it runs out of real codes and
 * walks into the padding. */
ZTEST(fcb_lzw, test_a_truncated_stream_does_not_produce_the_config)
{
	static uint8_t short_stream[64];

	memset(short_stream, 0xff, sizeof(short_stream));
	memset(decoded, 0, sizeof(decoded));
	emitted = 2U;
	int ret = fcb_lzw_decode(short_stream, CONFIG_WORDS - 2U, collect, NULL);

	/* Either it gives up (-EINVAL) or it decodes rubbish -- both are fine
	 * here, "the right bytes" is not. What matters is that the caller can
	 * tell the difference, and that is exactly the CRC check. */
	zassert_true(ret != 0 || crc32_ieee(decoded, sizeof(decoded)) != EXPECTED_CRC,
		     "a 64-byte stream cannot be the 99944-byte config");
}

ZTEST(fcb_lzw, test_rejects_a_stop_code)
{
	/* STOP (257) in the 9 bits the stream starts with: 0b100000001. */
	static const uint8_t stop[4] = { 0x80U, 0x80U, 0xffU, 0xffU };

	memset(decoded, 0, sizeof(decoded));
	emitted = 2U;
	zassert_equal(fcb_lzw_decode(stop, 4U, collect, NULL), -EINVAL,
		      "STOP before the expected word count is an error");
}

ZTEST(fcb_lzw, test_rejects_an_undefined_code)
{
	/* First code 300: no such dictionary entry yet (the decoder defines up
	 * to 258 on its first code). 300 = 0b1_0010_1100. */
	static const uint8_t bogus[4] = { 0x96U, 0x00U, 0x00U, 0x00U };

	memset(decoded, 0, sizeof(decoded));
	emitted = 2U;
	zassert_equal(fcb_lzw_decode(bogus, 4U, collect, NULL), -EINVAL,
		      "a code the stream has not defined is an error");
}

ZTEST(fcb_lzw, test_a_flipped_byte_changes_the_result)
{
	static uint8_t tampered[FIXTURE_MAX];

	zassert_true(agm_compressed_bitstream_len <= sizeof(tampered));
	memcpy(tampered, agm_compressed_bitstream, agm_compressed_bitstream_len);
	tampered[agm_compressed_bitstream_len / 2U] ^= 0xffU;

	memset(decoded, 0, sizeof(decoded));
	memcpy(decoded, tampered, 8U);
	emitted = 2U;
	int ret = fcb_lzw_decode(tampered + 8U, CONFIG_WORDS - 2U, collect, NULL);

	zassert_true(ret != 0 || crc32_ieee(decoded, sizeof(decoded)) != EXPECTED_CRC,
		     "a flipped byte must not decode to the same config");
}

ZTEST_SUITE(fcb_lzw, NULL, NULL, NULL, NULL, NULL);
