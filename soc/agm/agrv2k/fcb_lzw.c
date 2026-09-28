/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief LZW decoder for compressed AgRV2K bitstreams (see fcb_lzw.h).
 *
 * This is a port of FCB_AutoDecompressEncrypt() from the vendor SDK
 * (framework-agrv_sdk/src/fcb.c) with three changes, all of them about
 * running inside a Zephyr image rather than a vendor app:
 *
 *  1. the scratch state is static, not stack: the vendor version puts
 *     dict_index[]/dict_data[]/decode_array[] (about 6 KiB) on the stack, and
 *     this runs from soc.c's PRE_KERNEL_1 hook on the interrupt stack. Making
 *     them static costs 6 KiB of BSS and removes any dependence on
 *     CONFIG_ISR_STACK_SIZE. (It also means the function is not reentrant,
 *     which is fine: exactly one caller, once, at boot.)
 *  2. where the words go is the caller's business (fcb_lzw_emit_t) instead of
 *     being hard-wired to FCB->AUTO or an FCB_BUF macro, so the decoder is a
 *     pure function that tests/soc/agm/fcb_lzw can run on native_sim.
 *  3. no per-chip decryption (the vendor's `encrypt_decode` callback exists
 *     for board_logic.encrypt, where the downloader XORs the config with a
 *     chip-specific stream). That is a *product* decision, not a limitation
 *     of the device: the key schedule was read out of the host tool and the
 *     128-bit flash UID is readable by any code on the chip once the flash
 *     controller is unlocked, so a
 *     device-side decoder could be written. It is not, because that form buys
 *     per-chip *binding* rather than confidentiality, and the vendor's own
 *     documentation has an encrypted logic that disables remote updates:
 *     a board whose factory config is encrypted does not boot this port.
 *
 * The bit stream is read most significant bit first, codes widen 9 -> 10 -> 11
 * bits as the dictionary grows, and the first two words of the config are not
 * part of it (the caller writes those). tools/compress_bitstream.py is the
 * matching encoder; its --check mode and this decoder agree byte for byte on
 * the canonical 99944-byte bitstream, and the vendor's own decoder was run
 * against the same stream during bring-up (see the docs page).
 */

#include <errno.h>

#include "fcb_lzw.h"

/* Vendor constants (SDK fcb.c). */
#define LZW_DATA_WIDTH 8
#define MAX_DATA       ((1 << LZW_DATA_WIDTH) - 1) /* 255: literal codes */
#define CLEAR_INDEX    (MAX_DATA + 1)              /* 256 */
#define STOP_INDEX     (MAX_DATA + 2)              /* 257 */
#define MIN_INDEX      (MAX_DATA + 3)              /* 258: first dictionary code */
#define LZW_MAX_INDEX  1023                        /* dictionary slot limit */

static int dict_index[LZW_MAX_INDEX + 1];
static uint8_t dict_data[LZW_MAX_INDEX + 1];
static uint8_t entry[LZW_MAX_INDEX + 1];

int fcb_lzw_decode(const uint8_t *in, uint32_t out_words, fcb_lzw_emit_t emit,
		   void *ctx)
{
	uint32_t index_width = LZW_DATA_WIDTH + 1;
	uint32_t index_mask = (1U << index_width) - 1U;
	int decode_index = MIN_INDEX - MAX_DATA;
	int prev_index = -1;
	uint8_t prev_data0 = 0U;
	uint32_t fcb_word = 0U;
	uint32_t bytes = 0U;
	uint32_t words = 0U;
	uint32_t bit_count = 0U;
	uint32_t data = 0U;

	while (words < out_words) {
		uint32_t n;
		int decode_data;
		int index;

		while (bit_count < index_width) {
			data = (data << 8) | *in++;
			bit_count += 8U;
		}
		bit_count -= index_width;
		index = (int)((data >> bit_count) & index_mask);

		if (index == CLEAR_INDEX) {
			index_width = LZW_DATA_WIDTH + 1;
			index_mask = (1U << index_width) - 1U;
			decode_index = MIN_INDEX - MAX_DATA;
			prev_index = -1;
			continue;
		}
		if (index == STOP_INDEX) {
			return -EINVAL;
		}

		/* The code the decoder is about to define: it is only reachable
		 * through the "entry + its own first byte" case. */
		if (index == decode_index + MAX_DATA) {
			if (decode_index > LZW_MAX_INDEX) {
				return -EINVAL;
			}
			dict_index[decode_index] = prev_index;
			dict_data[decode_index] = prev_data0;
		}
		if (index > decode_index + MAX_DATA) {
			return -EINVAL; /* code the stream has not defined */
		}

		/* Walk the chain back to a literal, collecting the entry
		 * back-to-front (same shape as the SDK). */
		n = 0U;
		decode_data = index;
		for (;;) {
			if (decode_data <= MAX_DATA) {
				entry[n++] = (uint8_t)decode_data;
				prev_data0 = (uint8_t)decode_data;
				break;
			}
			decode_data -= MAX_DATA;
			if (decode_data < 0 || n >= sizeof(entry)) {
				return -EINVAL;
			}
			entry[n++] = dict_data[decode_data];
			decode_data = dict_index[decode_data];
		}

		if (prev_index >= 0) {
			if (decode_index > LZW_MAX_INDEX) {
				return -EINVAL;
			}
			dict_index[decode_index] = prev_index;
			dict_data[decode_index] = entry[n - 1U];
			decode_index++;
			if ((uint32_t)decode_index + MAX_DATA > index_mask) {
				index_width++;
				index_mask = (index_mask << 1) | 1U;
			}
		}
		prev_index = index;

		/* Emit the entry front-to-back, one byte at a time into the
		 * word being assembled. */
		while (n > 0U) {
			n--;
			fcb_word = (fcb_word >> 8) | ((uint32_t)entry[n] << 24);
			if (++bytes % 4U == 0U) {
				emit(ctx, fcb_word);
				if (++words == out_words) {
					return 0;
				}
			}
		}
	}

	return 0;
}
