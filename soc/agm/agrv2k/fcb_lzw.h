/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief LZW decoder for compressed AgRV2K bitstreams.
 *
 * A Supra config can be built compressed (`logic_compress = true` in the
 * vendor's PlatformIO projects, which passes `set LOGIC_COMPRESS true` to
 * Supra). The SDK decompresses it in software: FCB_AutoDecompress[Encrypt]()
 * in framework-agrv_sdk/src/fcb.c, and the ROM calls a compiled copy of the
 * same algorithm that the flashing tool writes next to the config. A Zephyr
 * image that streams the fabric itself therefore needs the same decoder, which
 * is what this file is -- see tools/compress_bitstream.py.
 */

#ifndef SOC_AGM_AGRV2K_FCB_LZW_H_
#define SOC_AGM_AGRV2K_FCB_LZW_H_

#include <stdint.h>

/** Sink for one decoded word, in the byte order the FCB's AUTO register takes
 *  raw config words in (little endian). */
typedef void (*fcb_lzw_emit_t)(void *ctx, uint32_t word);

/**
 * @brief Decode the LZW stream of a compressed bitstream.
 *
 * @a in points at the *first compressed byte*, i.e. 8 bytes into the config:
 * the vendor keeps IDCODE and USERID raw in front of the stream (the SDK's
 * decoder writes those two words out before it starts decoding, and its
 * per-chip encrypt path starts at the same offset for the same reason).
 *
 * @param in        compressed stream
 * @param out_words words to produce. The caller has already emitted the two
 *                  header words, so a full config is one FCB_AUTO_WORDS minus
 *                  two.
 * @param emit      called once per decoded word
 * @param ctx       passed through to @a emit
 *
 * @return 0, or -EINVAL when the stream carries an unknown code, a STOP before
 *         @a out_words, or an entry longer than the dictionary allows. A
 *         stream that simply *ends* early cannot be detected here (a config in
 *         flash has no length field): it reads whatever follows, and the FCB
 *         reports the result as ERR_HEADER/ERR_CRC at ACTIVATE.
 */
int fcb_lzw_decode(const uint8_t *in, uint32_t out_words, fcb_lzw_emit_t emit,
		   void *ctx);

#endif /* SOC_AGM_AGRV2K_FCB_LZW_H_ */
