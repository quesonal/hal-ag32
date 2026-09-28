#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# agm_logic_crypt.py — the vendor's per-chip "bitstream encryption", host side.
#
# What this is
# ------------
# `board_logic.encrypt = true` makes the vendor toolchain bind a Supra config
# to one chip: the config is LZW-compressed, then the LZW stream is turned into
# a key stream and XOR-rotated per byte, and the option bytes are marked
# `encrypted` so the ROM decrypts it again on the way into the FCB. The ROM can
# only stream *that* form, which is why an encrypted factory bitstream used to
# be a dead end for the A/B updater.
#
# This module is that transform, read out of the unstripped `agrv32flash`
# (encrypt.c + the `encrypt_salsa20.inc` it includes) and validated against the
# vendor's own functions -- see `--selftest`. Two
# properties to keep in mind before using it for anything:
#
#   * it is a *compatibility* tool, not a security boundary: the key material
#     is three XOR combinations of the SPI flash's 0x4B unique ID, which the
#     chip hands out to anyone who asks, and the cipher is a two-round Salsa20
#     variant with 13 of its 16 state words fixed for every chip;
#   * it only implements the *cipher*. Whether the device should re-seal a
#     plaintext image, or stream a vendor-encrypted factory region without
#     re-streaming it -- this
#     file just makes the bytes possible.
#
# The transform
# -------------
#   seed  = 0x326402ac   (hard-coded in the tool; NOT read from the chip)
#   MWC   = z0 = 36969*(z0 & 0xffff) + (z0 >> 16), same for z1 with 18000,
#           one word = (z0 << 16) | (z1 & 0xffff); seeded z0 = seed ^ 0x3c99de82,
#           z1 = seed ^ 0xcfd2af98
#   state = 16 words: [0]="expe", [1..3]=0, [4]=key0, [5..7]=MWC, [8]=vid,
#           [9]=MWC, [10]="6-wo", [11]=MWC, [12]=key1, [13]=MWC, [14]=key3,
#           [15]="rds!"       (the *four* custom words are "expect 16-words!",
#                              the two constants that survive are shown here)
#   block = ONE Salsa20 double round over the state (column round on
#           (0,4,8,12)/(5,9,13,1)/(10,14,2,6)/(15,3,7,11), then row round on
#           (0,1,2,3)/(5,6,7,4)/(10,11,8,9)/(15,12,13,14), rotations 7/9/13/18),
#           and the state becomes state + block (so blocks chain)
#   byte  = rotl8(plain ^ block[i & 63], (i & 7) ^ 6)
#
# `key` above is what the tool calls the flash key: the 16 bytes read from the
# SPI NOR with command 0x4B, combined as
#
#   flash_key = [u0^u1, u1^u2, u2^u3, u3^(u0^u1)]     (u = the four UID words)
#
# Note `flash_key[2]` — the vendor code stores it and then overwrites it, in the
# host tool *and* in the device-side blob, so that one combination (u2 ^ u3)
# never reaches the key stream. All four UID words still do, because u2 and u3
# also appear in `flash_key[1]` / `flash_key[3]`. `--selftest` pins both halves.

import argparse
import struct
import sys

M32 = 0xFFFFFFFF

# Hard-coded "vid" in agrv32flash: encrypt_encode() calls
# encrypt_getvid(0x326402ac, 0).
VID = 0x326402AC

# The four custom Salsa20 constants: the string "expect 16-words!" read as
# little-endian words and placed at x[0], x[5], x[10], x[15] (where Salsa20
# puts "expand 32-byte k"). Only the ones at [10] and [15] survive; [5] is
# overwritten by an MWC word below, and the tool's own table writes all four.
_CONSTANTS = b"expect 16-words!"


def rotl32(value, n):
    return ((value << n) | (value >> (32 - n))) & M32


def rotl8(value, n):
    return ((value << n) | (value >> (8 - n))) & 0xFF


def ror8(value, n):
    return ((value >> n) | (value << (8 - n))) & 0xFF


class _Mwc:
    """encrypt_rand(): Marsaglia multiply-with-carry."""

    def __init__(self, seed):
        self.z0 = seed ^ 0x3C99DE82
        self.z1 = seed ^ 0xCFD2AF98

    def next(self):
        self.z0 = (36969 * (self.z0 & 0xFFFF) + (self.z0 >> 16)) & M32
        self.z1 = (18000 * (self.z1 & 0xFFFF) + (self.z1 >> 16)) & M32
        return ((self.z0 << 16) | (self.z1 & 0xFFFF)) & M32


def uid_to_flash_key(uid):
    """agrv32_get_flash_key()'s XOR chain over the 16 bytes read with 0x4B."""
    if len(uid) != 16:
        raise ValueError("the UID is 16 bytes (four 0x4B flex reads), got %d" % len(uid))

    key = list(struct.unpack("<4I", uid))
    key[0] ^= key[1]
    key[1] ^= key[2]
    key[2] ^= key[3]
    key[3] ^= key[0]
    return key


def _getvid():
    return _Mwc(VID).next()


def _setup(key):
    rng = _Mwc(_getvid())
    x = [0] * 16

    x[0] = struct.unpack("<I", _CONSTANTS[0:4])[0]
    x[5] = struct.unpack("<I", _CONSTANTS[4:8])[0]
    x[10] = struct.unpack("<I", _CONSTANTS[8:12])[0]
    x[15] = struct.unpack("<I", _CONSTANTS[12:16])[0]

    # The write order matters: x[5] and x[6] are written here and overwritten
    # by the MWC words below, exactly as the vendor code does it.
    x[4] = key[0]
    x[5] = rng.next()
    x[6] = key[2]
    x[7] = rng.next()
    x[11] = rng.next()
    x[12] = key[1]
    x[13] = rng.next()
    x[14] = key[3]
    x[6] = rng.next()
    x[7] = rng.next()
    x[8] = _getvid()
    x[9] = rng.next()
    return x


def _quarterround(z, a, b, c, d):
    z[b] ^= rotl32((z[a] + z[d]) & M32, 7)
    z[c] ^= rotl32((z[b] + z[a]) & M32, 9)
    z[d] ^= rotl32((z[c] + z[b]) & M32, 13)
    z[a] ^= rotl32((z[d] + z[c]) & M32, 18)


def _core(state):
    """One Salsa20 double round (encrypt_keygen runs its body once)."""
    z = list(state)
    for a, b, c, d in ((0, 4, 8, 12), (5, 9, 13, 1), (10, 14, 2, 6), (15, 3, 7, 11)):
        _quarterround(z, a, b, c, d)
    for a, b, c, d in ((0, 1, 2, 3), (5, 6, 7, 4), (10, 11, 8, 9), (15, 12, 13, 14)):
        _quarterround(z, a, b, c, d)
    return z


def crypt(data, key, decrypt=False):
    """encrypt_process_encode/decode(): rotate+XOR against the chained state.

    `key` is the four-word flash key (see uid_to_flash_key). The transform is
    its own inverse (`decrypt=True` only selects the other 8-bit rotation, i.e.
    the vendor's other function).
    """
    state = _setup(key)
    out = bytearray(len(data))
    block = b""

    for i, byte in enumerate(data):
        if (i & 0x3F) == 0:
            z = _core(state)
            block = b"".join(struct.pack("<I", w) for w in z)
            for j in range(16):
                state[j] = (state[j] + z[j]) & M32
        n = (i & 7) ^ 6
        if decrypt:
            out[i] = block[i & 0x3F] ^ ror8(byte, n)
        else:
            out[i] = rotl8(byte ^ block[i & 0x3F], n)
    return bytes(out)


def crypt_bitstream(data, uid, offset=8, length=None, decrypt=False):
    """Apply the vendor's convention to a whole config/bitstream image.

    The vendor encrypts the config from +8 on: bytes 0..7 are IDCODE/USERID and
    stay raw (that is the same +8 the compressed form uses -- see
    tools/compress_bitstream.py). Pass offset=0/length=None to transform a
    region the caller has already cut out, and decrypt=True to run the inverse
    (`encrypt_decode`, which rotates the other way).
    """
    if length is None:
        length = len(data) - offset
    body = list(data)
    body[offset:offset + length] = crypt(bytes(data[offset:offset + length]),
                         uid_to_flash_key(uid), decrypt=decrypt)
    return bytes(body)


# --- known-answer vectors, taken from the vendor binary itself.
# Plaintext byte i is (i*7+3) & 0xff.
_KAT = (
    # flash_key, length, ciphertext
    ("444444cc cccccc44 444444cc 99aabbcc", 32,
     "664799aadc51b875ede1711d752624819125014b26d04d70c65043493bffa249"),
    ("a1b2c3d4 5e6f7081 92a3b4c5 d6e7f809", 65,
     "c98878bb423d4084202ea4a7b7809b4bb886e57751f9de968b4ad3453a404a25"
     "b62e1fede20e1629e9e9de31a44dd890929a4cef6639dea0c2f9737ebd316511e9"),
    ("444444cc cccccc44 444444cc 99aabbcc", 999,
     "664799aadc51b875ede1711d752624819125014b26d04d70c65043493bffa249"
     "b9579df118942978a8bb263f7eb70787c72303e8a3ee6473b2101a9f81179f64b5"
     "a4eac7ccc02c3b7d5cae41abf0fa4797ec980d292227fc80c399b75ac369a73cea"
     "45268dccc0d471761aa198a1cc1d156967eedbe4086c775f24567ef2f725c8242537"
     "c29213cc282671787ebdf3b405629a4aab0d012b4b78a2f5b40ad0e167246127f8d"
     "6ec4c80f72cb229cae9c9b4b0858b8adc887f796d1f83d0d73b5a647f5183e8523"
     "3d8511536faa5025b39d0358f3fbe8f5c49f1b00a874c61b29b94331e27f3bb874"
     "2b62d78800e944bc747da70d7ff242693000fcab2c8b2b5aaf50f3506ebd6d5152"
     "7c0194a95f5712eac41fba0844412143b0a8073fe3adc0a7b462b1d9bd169da130"
     "3f73f55f8fe46bc8ddbc79ddd2120e6aed95fcb3ff7185eff3da70f7ea78e1ecfd"
     "f55e9365968a6500ebb180917e57d18a4e2ccb12db081e71a66dd013d4454175db"
     "aaeeaf239a28e655667bae5a9dcacb79b519cd61d1714de4a371799c0d27acb55e"
     "791fcb2db9d6a7643940d4cfb63f03e6279872663503b67ad6591eaf03b638e337"
     "e4518cf691db52774a8d239b57cb7000a012e595b1b9361345201798f55918ed9d"
     "69e56f50cb867eb144354e0f3d32d8e0dcf5c641d433f11e42fe37b99d7291802c"
     "1c6851f2a7a7da417c066036a5f0501f4269fbf1a4cdc7dbdb5cb1849f0b063a95"
     "f1a005850465c13d066b098d482b4ca6c2ae177ac7a2b8e1b488f89f069b6f6468"
     "d9daebaae40d8d895534fa6f9738b39133c9c6855cc79446191b85dd6159756afe"
     "ba0a6271c83543fbdfe1114131d7b8bedf6364e70915331e0153d86a24d3e6a41c"
     "44c775a9b4d6ae1c58a47009ba87e065187091a5786f572365e387308364a0f833"
     "8505fce050bcd3f447b82cc59d3c6eb4c577c81bf9ea362074e0406775628bb6ed"
     "7d1e8731a920ad691d87509e34d3a9e793b05e67f9eb585fcde497b3f9c094ab46"
     "68e803ae0e7c92a9a6ba4aa489a8f009b6544c19991f16d9037b3e97124d090ab4"
     "21c5b492894e7f0eb90cf71eec987b405d1f7a48d8974732e09f87b6d779aba054"
     "3b7138c9b89bead1919b5a6edaab4d1b0dbb393ae305c2d46818f9fffd18077162"
     "da27aeed7fee7b5cfc68dfe56a0fa5982168b62b3d22edb406294971eb884f21de"
     "a5e23cb3cea3434d7f041c2a112ff6d3d9520d64742bffc27e8d51cb39eab9c075"
     "5831d9f08cadf55a7bb679d9072fad6c0399bc06ea91b673ef8184dfaafbc2c3f1"
     "e6a2b6248c3c7e6600c8b9990f432ff5aca337daf57cdff07515646ef641a58572"
     "48fc0e5794e1b677ff6bd6abee4633d062134ae1788fe87afa24ee534a6f4568c8"
     "a919f50be46ba763f"),
)


def selftest():
    """Check the implementation against the vendor's own output."""
    for text, length, want in _KAT:
        key = [int(w, 16) for w in text.split()]
        plain = bytes(((i * 7 + 3) & 0xFF) for i in range(length))
        got = crypt(plain, key)
        if got.hex() != want:
            raise SystemExit("selftest failed for len=%d:\n got %s\nwant %s"
                     % (length, got.hex(), want))
        if crypt(got, key, decrypt=True) != plain:
            raise SystemExit("selftest failed: decode(%d) is not the inverse" % length)

    # `flash_key[2]` is a dead store in the vendor code (pinned against the
    # vendor binary: two key sets differing only in that word produce the same
    # ciphertext). Every *UID* word, on the other hand, does reach the key
    # stream -- u2 and u3 through the other three combinations.
    base = [0x01020304, 0x05060708, 0x090A0B0C, 0x0D0E0F10]
    plain = bytes(range(64))
    dead = list(base)
    dead[2] ^= 0xFFFFFFFF
    if crypt(plain, base) != crypt(plain, dead):
        raise SystemExit("selftest failed: flash_key[2] reaches the key stream")
    for word in (0, 1, 3):
        changed = list(base)
        changed[word] ^= 0xFFFFFFFF
        if crypt(plain, base) == crypt(plain, changed):
            raise SystemExit("selftest failed: flash_key[%d] is a dead store" % word)

    uid = bytes.fromhex("112233445566778899aabbccddeeff00")
    for word in range(4):
        changed = bytearray(uid)
        changed[4 * word:4 * word + 4] = b"\x01\x02\x03\x04"
        if crypt(plain, uid_to_flash_key(uid)) == crypt(plain, uid_to_flash_key(bytes(changed))):
            raise SystemExit("selftest failed: UID word %d does not reach the key stream" % word)

    print("selftest: %d known-answer vectors from the vendor binary, UID-word "
          "sensitivity, round trip -- OK" % len(_KAT))


def _read_uid(path, text):
    if path:
        with open(path, "rb") as handle:
            uid = handle.read()
    elif text:
        try:
            uid = bytes.fromhex(text)
        except ValueError:
            raise SystemExit("--uid must be hex bytes, e.g. 1122334455667788...")
    else:
        raise SystemExit("give --uid <hex> or --uid-file <path>")
    if len(uid) != 16:
        raise SystemExit("the UID is 16 bytes, got %d" % len(uid))
    return uid


def main(argv):
    parser = argparse.ArgumentParser(
        description="The vendor's per-chip bitstream cipher (host side). "
                "`encrypt` and `decrypt` are the vendor's two mirror calls: they "
                "differ in the direction of the per-byte 8-bit rotation.")
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("selftest", help="check this implementation against the "
                    "vendor binary's known-answer vectors")

    keys = sub.add_parser("keys", help="derive the flash key from a 16-byte UID")
    keys.add_argument("--uid", help="16 bytes as hex")
    keys.add_argument("--uid-file", help="16 raw bytes read from a file")

    crypt_cmd = sub.add_parser("crypt", aliases=["encrypt", "decrypt"],
                   help="encrypt/decrypt a bitstream region")
    crypt_cmd.add_argument("infile")
    crypt_cmd.add_argument("-o", "--out", required=True)
    crypt_cmd.add_argument("--uid", help="16 bytes as hex")
    crypt_cmd.add_argument("--uid-file", help="16 raw bytes read from a file")
    crypt_cmd.add_argument("--offset", type=lambda v: int(v, 0), default=8,
                   help="first byte to transform (default 8: the vendor "
                    "leaves IDCODE/USERID raw)")
    crypt_cmd.add_argument("--length", type=lambda v: int(v, 0),
                   help="bytes to transform (default: to the end)")

    args = parser.parse_args(argv)

    if args.cmd == "selftest":
        selftest()
        return 0

    uid = _read_uid(getattr(args, "uid_file", None), getattr(args, "uid", None))

    if args.cmd == "keys":
        print("flash_key: " + " ".join("%08x" % w for w in uid_to_flash_key(uid)))
        return 0

    with open(args.infile, "rb") as handle:
        data = handle.read()
    out = crypt_bitstream(data, uid, args.offset, args.length,
                  decrypt=args.cmd == "decrypt")
    with open(args.out, "wb") as handle:
        handle.write(out)
    print("%s: %d B, transformed [%d, %d)"
          % (args.out, len(out), args.offset,
         args.offset + (len(data) - args.offset if args.length is None else args.length)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
