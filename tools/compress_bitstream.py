#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# compress_bitstream.py — LZW-compress a Supra bitstream the way the AgRV2K
# boot path expects to find it (the SDK's `logic_compress = true` form).
#
# Why this exists
# ---------------
# A Supra config is 99944 bytes (FCB_AUTO_WORDS = 24986 words) and the chip
# keeps 100 KiB of flash for it. The SDK can build the *compressed* variant
# instead (platformio.ini: `logic_compress = true` -> Supra gets
# `set LOGIC_COMPRESS true`), and then the decompression algorithm has to run
# on the RISC-V side: the SDK firmware calls FCB_AutoDecompress(), the ROM
# calls the copy of that algorithm the flashing tool writes next to the
# config, and this port can be built with CONFIG_AGM_FCB_BITSTREAM_COMPRESSED
# to do the same.
#
# Supra only emits that form as part of a Quartus flow, so this tool exists to
# produce the same stream from an already-built config (and to generate the
# test vectors for tests/soc/agm/fcb_lzw). The format is defined by the
# decoder both sides implement -- framework-agrv_sdk/src/fcb.c:
#
#   bytes 0..7    : IDCODE, USERID -- NOT compressed (and not encrypted)
#   bytes 8..     : LZW bit stream, MSB first, decoding to the original
#                   bytes 8..99944
#
# The first two words stay raw because the decoder writes them out before it
# starts decoding (and the vendor's encrypt path starts at +8 for the same
# reason). A decoder is included below (`--check`, and the tests use it) so a
# change to the encoder cannot silently produce a stream nothing can read.

import argparse
import sys

# Same constants as the decoder (SDK fcb.c: LZW_DATA_WIDTH / MAX_DATA /
# CLEAR_INDEX / STOP_INDEX / MIN_INDEX, and FCB_AUTO_WORDS).
LZW_DATA_WIDTH = 8
MAX_DATA = (1 << LZW_DATA_WIDTH) - 1        # 255: literal codes
CLEAR_INDEX = MAX_DATA + 1                  # 256
STOP_INDEX = MAX_DATA + 2                   # 257
MIN_INDEX = MAX_DATA + 3                    # 258: first dictionary code
LZW_MAX_INDEX = 1023                        # dictionary slot limit (decoder's
#                                             dict_index[]/dict_data[] size - 1)
FCB_AUTO_WORDS = 24986
CONFIG_BYTES = FCB_AUTO_WORDS * 4           # 99944
HEADER_BYTES = 8                            # IDCODE + USERID stay raw


class BitWriter:
    def __init__(self):
        self.buf = bytearray()
        self.acc = 0
        self.bits = 0

    def put(self, code, width):
        self.acc = (self.acc << width) | (code & ((1 << width) - 1))
        self.bits += width
        while self.bits >= 8:
            self.bits -= 8
            self.buf.append((self.acc >> self.bits) & 0xFF)

    def flush(self):
        if self.bits:
            # Trailing bits are never read back: the decoder stops when it has
            # produced the expected number of bytes. 0xFF matches erased flash.
            self.buf.append(((self.acc << (8 - self.bits)) | (0xFF >> self.bits)) & 0xFF)
            self.bits = 0
        return bytes(self.buf)


class BitReader:
    def __init__(self, data):
        self.data = data
        self.pos = 0
        self.acc = 0
        self.bits = 0

    def get(self, width):
        while self.bits < width:
            byte = self.data[self.pos] if self.pos < len(self.data) else 0xFF
            self.pos += 1
            self.acc = ((self.acc << 8) | byte) & 0xFFFFFFFF
            self.bits += 8
        self.bits -= width
        return (self.acc >> self.bits) & ((1 << width) - 1)


def compress(config):
    """config -> header (raw) + LZW stream of the rest."""
    if len(config) != CONFIG_BYTES:
        raise ValueError("expected %d bytes, got %d (not a Supra config)"
                         % (CONFIG_BYTES, len(config)))
    body = config[HEADER_BYTES:]
    w = BitWriter()

    def fresh():
        # Literal entries 0..255; MIN_INDEX (258) is the first code the decoder
        # will add, CLEAR_INDEX/STOP_INDEX (256/257) are not entries.
        return {bytes([i]): i for i in range(MAX_DATA + 1)}

    # The decoder's own state, mirrored: the *input* side must use exactly the
    # bit width the decoder will use for this code, and widen at the same
    # point. The decoder pairs the previous code with the first byte of the
    # current one, so its entry counter runs one code behind the encoder's
    # dictionary -- deriving the width from the encoder's counter is off by
    # one at every step (the first 9->10 step came out one code
    # early). Mirroring removes the question.
    mirror = {"width": LZW_DATA_WIDTH + 1, "dec_index": MIN_INDEX - MAX_DATA,
              "have_prev": False}

    def put(code):
        w.put(code, mirror["width"])
        if code == CLEAR_INDEX:
            mirror.update(width=LZW_DATA_WIDTH + 1,
                          dec_index=MIN_INDEX - MAX_DATA, have_prev=False)
            return
        if mirror["have_prev"]:
            mirror["dec_index"] += 1
            if mirror["dec_index"] + MAX_DATA > (1 << mirror["width"]) - 1:
                mirror["width"] += 1
        mirror["have_prev"] = True

    table = fresh()
    next_code = MIN_INDEX

    cur = bytes(body[:1])
    for byte in body[1:]:
        nxt = cur + bytes([byte])
        if nxt in table:
            cur = nxt
            continue

        put(table[cur])
        # The decoder adds one entry per code (after its first), so the
        # encoder has to add one per emitted code too or the two tables drift.
        if next_code <= LZW_MAX_INDEX:
            table[nxt] = next_code
            next_code += 1
        else:
            # Dictionary full: CLEAR both sides and start over from the
            # current byte. The decoder's dict_index[]/dict_data[] are sized
            # for LZW_MAX_INDEX, so a stream that never clears would run off
            # the end of the vendor's arrays.
            put(CLEAR_INDEX)
            table = fresh()
            next_code = MIN_INDEX
        cur = bytes([byte])

    put(table[cur])
    return config[:HEADER_BYTES] + w.flush()


def decompress(blob):
    """The decoder both the SDK ('FCB_AutoDecompress') and the ROM implement.

    This is a direct transcription of framework-agrv_sdk/src/fcb.c so the
    encoder can be checked against it without a board; the C port in
    soc/agm/agrv2k/fcb_lzw.c follows the same code.
    """
    if len(blob) < HEADER_BYTES:
        raise ValueError("short image")
    out = bytearray(blob[:HEADER_BYTES])
    want = CONFIG_BYTES - HEADER_BYTES
    r = BitReader(blob[HEADER_BYTES:])

    index_width = LZW_DATA_WIDTH + 1
    index_mask = (1 << index_width) - 1
    decode_index = MIN_INDEX - MAX_DATA
    prev_index = -1
    prev_data0 = 0
    dict_index = [0] * (LZW_MAX_INDEX + 1)
    dict_data = [0] * (LZW_MAX_INDEX + 1)

    while len(out) - HEADER_BYTES < want:
        index = r.get(index_width)
        if index == CLEAR_INDEX:
            index_width = LZW_DATA_WIDTH + 1
            index_mask = (1 << index_width) - 1
            decode_index = MIN_INDEX - MAX_DATA
            prev_index = -1
            continue
        if index == STOP_INDEX:
            raise ValueError("STOP code before the expected byte count")
        if index == decode_index + MAX_DATA:
            dict_index[decode_index] = prev_index
            dict_data[decode_index] = prev_data0
        if index > decode_index + MAX_DATA:
            raise ValueError("unknown index %d" % index)

        entry = bytearray()
        decode_data = index
        while True:
            if decode_data <= MAX_DATA:
                entry.append(decode_data)
                prev_data0 = decode_data
                break
            decode_data -= MAX_DATA
            entry.append(dict_data[decode_data])
            decode_data = dict_index[decode_data]
        entry.reverse()

        if prev_index >= 0:
            dict_index[decode_index] = prev_index
            dict_data[decode_index] = entry[0]
            decode_index += 1
            if decode_index + MAX_DATA > index_mask:
                index_width += 1
                index_mask = (index_mask << 1) | 1
        prev_index = index
        out += entry

    return bytes(out)


def main(argv):
    ap = argparse.ArgumentParser(
        description="LZW-compress a Supra bitstream for CONFIG_AGM_FCB_"
                    "BITSTREAM_COMPRESSED")
    ap.add_argument("image", help="uncompressed Supra bitstream (99944 B)")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--check", action="store_true",
                    help="decode the result again and compare (always worth it: "
                         "a stream nothing can read is a dead board)")
    args = ap.parse_args(argv)

    config = open(args.image, "rb").read()
    try:
        blob = compress(config)
    except ValueError as exc:
        sys.exit(str(exc))
    open(args.output, "wb").write(blob)

    print("compressed: %s (%d B -> %d B, %.1f%%)"
          % (args.output, len(config), len(blob), 100.0 * len(blob) / len(config)))

    if args.check:
        back = decompress(blob)
        if back != config:
            sys.exit("self-check FAILED: decompressed image differs from %s"
                     % args.image)
        print("self-check: decodes back to the input, byte for byte")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
