#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# sign_image.py — sign an application image for the AGM bootloader.
#
# The bootloader's signed modes accept only MCUboot containers, so that is what
# this produces. It is a thin, opinionated wrapper around imgtool (which ships
# with the mcuboot module and is installed in this repository's virtualenv) that
# also emits the public key the loader has to be built with -- and that then
# checks its own output, so a broken invocation fails here instead of on the
# dev board:
#
#   tools/sign_image.py app.bin key.pem -o app.signed.bin --pubkey-out app.pub
#   west build -b agrv2k_407 samples/spi_boot_loader -- \
#       -DSPI_BOOT_PUBKEY=app.pub -DCONFIG_BOOT_AGM_SIG_ECDSA_P256=y
#   tools/agm_upload.py /dev/ttyACM1 a app.signed.bin
#
#   # a fabric image for the bitstream A/B slots (CONFIG_BOOT_AGM_BITSTREAM_SIGNED)
#   tools/sign_image.py --bitstream example_board.bin key.pem -o bs.signed.bin
#   tools/agm_upload.py /dev/ttyACM0 bitstream bs.signed.bin
#
# Both profiles go through here and the key type is taken from the key file, so
# there is no `--profile` flag to get out of sync with the key: an ECDSA P-256
# key produces the raw 64-byte X||Y the loader keeps and the SubjectPublicKeyInfo
# hash imgtool puts in its KEYHASH TLV, an RSA key produces the 270-byte PKCS#1
# DER and the PKCS#1 hash. (`-DCONFIG_BOOT_AGM_SIG_RSA2048_PSS=y` selects the
# RSA build; the loader then wants exactly that DER blob.)
#
# The application has to be linked so that it runs at `slot + header size`:
# build it with -DCONFIG_ROM_START_OFFSET=0x20 (the header-sized gap imgtool
# writes into). If it was not built that way, pass --pad-header and this tool
# inserts the gap -- imgtool refuses the combination that would double it.
#
# `--bitstream` signs a *fabric* image for the two update slots of the
# bitstream A/B pair instead (`CONFIG_BOOT_AGM_BITSTREAM_SIGNED`): same
# container, but the payload is the whole 99944-byte fabric, the run address is
# the fabric's canonical stamp (the slot 1 base + header, see
# include/zephyr/drivers/misc/agm_bitstream.h) and the slot is the 100 KiB
# reservation. The loader verifies it with ECDSA P-256 whatever the
# application profile is, so sign it with an ECDSA key.

import argparse
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa

# The fabric the FCB streams, and the slot it lives in (must match
# include/zephyr/drivers/misc/agm_bitstream.h).
BITSTREAM_IMAGE_LEN = 99944
BITSTREAM_SLOT_SIZE = 0x19000
BITSTREAM_SLOT_BASE = 0x800b4000

# The application slot of the default on-die layout, what the loader's board
# overlay points the store at.
APP_SLOT_BASE = 0x8007c000
APP_SLOT_SIZE = 0x80000


def is_rsa(key):
    return isinstance(key, rsa.RSAPrivateKey)


def pubkey_blob(private_key_path):
    """The bytes the loader keeps as `agm_boot_pubkey[]`, which are also the
    bytes its KEYHASH is taken over for this key type.

    imgtool hashes the SubjectPublicKeyInfo DER for ECDSA (its
    `keys/ecdsa.py:get_public_bytes`) but the PKCS#1 `RSAPublicKey` DER for RSA
    (`keys/rsa.py`), so the two profiles emit and hash *different* encodings.
    The sizes are what `AGM_BOOT_PUBKEY_LEN` in
    include/zephyr/drivers/misc/boot_agm.h expects: 64 and 270 bytes."""
    key = serialization.load_pem_private_key(open(private_key_path, "rb").read(),
                                             password=None)
    pub = key.public_key()

    if is_rsa(key):
        return pub.public_bytes(encoding=serialization.Encoding.DER,
                                format=serialization.PublicFormat.PKCS1)
    nums = pub.public_numbers()
    return nums.x.to_bytes(32, "big") + nums.y.to_bytes(32, "big")


def key_hash_of(private_key_path):
    """SHA-256 of the encoding imgtool puts in IMAGE_TLV_KEYHASH: the
    SubjectPublicKeyInfo DER for ECDSA, the PKCS#1 DER for RSA."""
    import hashlib

    key = serialization.load_pem_private_key(open(private_key_path, "rb").read(),
                                             password=None)
    if is_rsa(key):
        der = key.public_key().public_bytes(
            encoding=serialization.Encoding.DER,
            format=serialization.PublicFormat.PKCS1)
    else:
        der = key.public_key().public_bytes(
            encoding=serialization.Encoding.DER,
            format=serialization.PublicFormat.SubjectPublicKeyInfo)
    return hashlib.sha256(der).digest()


def _imgtool():
    """imgtool is a package, not a `python -m` entry point; import its CLI."""
    try:
        from imgtool import main as imgtool_main
    except ImportError:
        sys.exit("imgtool is not installed: run inside the repository virtualenv "
                 "(activate your venv, or `pip install imgtool`)")
    return imgtool_main


def run_imgtool(args):
    imgtool_main = _imgtool()
    try:
        imgtool_main.imgtool.main(list(args), standalone_mode=True)
    except SystemExit as exc:
        if exc.code:
            raise SystemExit("imgtool %s failed (%s)" % (" ".join(args), exc.code))


def read_header(container):
    """The fields of the MCUboot header the loader cares about."""
    import struct

    magic, load_addr, hdr_size, prot_size, img_size = struct.unpack_from("<IIHHI",
                                                                        container, 0)
    return {
        "magic": magic,
        "load_addr": load_addr,
        "hdr_size": hdr_size,
        "prot_tlv_size": prot_size,
        "img_size": img_size,
    }


def main(argv):
    ap = argparse.ArgumentParser(description="sign an image for the AGM bootloader")
    ap.add_argument("image", help="raw application image (zephyr.bin)")
    ap.add_argument("key", help="private key from imgtool keygen (ecdsa-p256 or "
                                "rsa-2048); its type picks the profile")
    ap.add_argument("-o", "--output", required=True, help="signed container to write")
    ap.add_argument("--pubkey-out", help="write the public key blob the loader "
                                         "is built with (SPI_BOOT_PUBKEY): the raw "
                                         "64-byte X||Y for ECDSA, the 270-byte "
                                         "PKCS#1 DER for RSA")
    ap.add_argument("--version", default="1.0.0")
    ap.add_argument("--bitstream", action="store_true",
                    help="sign a fabric (bitstream) image for the bitstream "
                         "slots: fixes --slot-base/--slot-size to the fabric "
                         "layout and requires the input to be exactly %d B"
                         % BITSTREAM_IMAGE_LEN)
    ap.add_argument("--slot-size", default=None,
                    help="slot size the container has to fit (default 512 KiB "
                         "for an application image, the 100 KiB fabric "
                         "reservation with --bitstream)")
    ap.add_argument("--header-size", default="0x20")
    ap.add_argument("--align", default="8")
    ap.add_argument("--slot-base", default=None,
                    help="address of the application slot the image will run "
                         "in; the header records slot-base + header-size as "
                         "ih_load_addr, and the loader refuses a container "
                         "whose header does not say where it will run it "
                         "(default: the application slot of the default "
                         "on-die layout, see samples/spi_boot_loader/boards/"
                         "agrv2k_407.overlay; with --bitstream: the fabric's "
                         "canonical stamp, 0x800b4020)")
    ap.add_argument("--load-addr",
                    help="ih_load_addr override; it has to be slot-base + "
                         "header-size, so this only exists to make the check "
                         "explicit")
    ap.add_argument("--pad-header", action="store_true",
                    help="insert the header-sized gap (for images not built with "
                         "CONFIG_ROM_START_OFFSET)")
    args = ap.parse_args(argv)

    image = open(args.image, "rb").read()
    hdr_size = int(args.header_size, 0)

    if args.bitstream:
        # One stamp for both fabric slots (they are interchangeable, and the
        # loader accepts only this one), one slot size, and the fabric's exact
        # length -- a bitstream of another size would be streamed into the FCB
        # as garbage, and the loader refuses it anyway.
        if args.slot_base is not None and int(args.slot_base, 0) != BITSTREAM_SLOT_BASE:
            sys.exit("--slot-base 0x%x is not the fabric's stamp (0x%x): a "
                     "fabric container is stamped for 0x%x, see "
                     "include/zephyr/drivers/misc/agm_bitstream.h"
                     % (int(args.slot_base, 0), BITSTREAM_SLOT_BASE,
                        BITSTREAM_SLOT_BASE))
        if args.slot_size is not None and int(args.slot_size, 0) != BITSTREAM_SLOT_SIZE:
            sys.exit("--slot-size 0x%x is not the fabric reservation (0x%x)"
                     % (int(args.slot_size, 0), BITSTREAM_SLOT_SIZE))
        if len(image) != BITSTREAM_IMAGE_LEN:
            sys.exit("%s is %u B, but a fabric image is exactly %u B (the "
                     "vendor SDK's FCB_AUTO_WORDS = 24986 words)"
                     % (args.image, len(image), BITSTREAM_IMAGE_LEN))
        slot_base = BITSTREAM_SLOT_BASE
        slot_size = BITSTREAM_SLOT_SIZE
    else:
        slot_base = int(args.slot_base, 0) if args.slot_base is not None else APP_SLOT_BASE
        slot_size = int(args.slot_size, 0) if args.slot_size is not None else APP_SLOT_SIZE

    # The payload sits behind the header, so that is where it has to be linked
    # -- and where the loader will look for it (`entry = slot + hdr_size`).
    load_addr = slot_base + hdr_size

    # The address ends up in a 32-bit header field and in the loader's entry
    # check, so refuse the values that cannot mean anything before imgtool
    # wraps them: a negative or oversized slot base, one that is not word
    # aligned (the loader's entry check would reject the container), and the
    # one that only overflows *after* adding the header -- `--slot-base
    # 0xfffffff0` is the interesting case, because it looks like a plain
    # address until the header is added .
    if slot_base < 0 or slot_base > 0xffffffff:
        sys.exit("--slot-base 0x%x is not a 32-bit address" % slot_base)
    if slot_base % 4 != 0:
        sys.exit("--slot-base 0x%x is not word aligned (the loader refuses an "
                 "unaligned entry)" % slot_base)
    if load_addr > 0xffffffff:
        sys.exit("--slot-base 0x%x + header size 0x%x overflows 32 bits "
                 "(0x%x): the header cannot record that address"
                 % (slot_base, hdr_size, load_addr))
    if args.load_addr is not None and int(args.load_addr, 0) != load_addr:
        sys.exit("--load-addr is 0x%x but this image would run at 0x%x "
                 "(slot-base 0x%x + header 0x%x): the loader refuses the "
                 "mismatch, so there is no point writing it"
                 % (int(args.load_addr, 0), load_addr, slot_base, hdr_size))

    # imgtool refuses to pad an image that already begins with the gap, and
    # refuses to write a header into one that does not: pick the right one here
    # so the payload always ends up at `slot + header size`.
    pad = args.pad_header or image[:hdr_size] != b"\x00" * hdr_size

    cmd = ["sign", "-k", args.key, "--header-size", args.header_size,
           "--align", args.align, "--slot-size", hex(slot_size),
           "--version", args.version, "--load-addr", hex(load_addr)]
    if args.bitstream:
        # MCUboot's swap trailer (128 sectors x 3 x 8 B = 3 KB by default) is
        # a reservation for a swap state machine this loader does not have: it
        # writes the *inactive* slot and points the boot record at it, which is
        # exactly imgtool's overwrite-only model. The fabric fills its 100 KiB
        # slot, so that reservation has to go or imgtool refuses the image.
        # (It only sizes the trailer; the container bytes are the same either
        # way, and no trailer is written without --pad.)
        cmd.append("--overwrite-only")
    if pad:
        cmd.append("--pad-header")
    cmd += [args.image, args.output]
    run_imgtool(cmd)

    container = open(args.output, "rb").read()
    hdr = read_header(container)
    if hdr["magic"] != 0x96F3B83D:
        sys.exit("imgtool produced no MCUboot header (magic 0x%08x)" % hdr["magic"])
    if hdr["load_addr"] != load_addr:
        sys.exit("imgtool wrote ih_load_addr=0x%x, expected 0x%x -- the loader "
                 "would refuse this container" % (hdr["load_addr"], load_addr))
    if len(container) != hdr["hdr_size"] + hdr["img_size"] + 4 + 0x98 - 0x20:
        # The TLV area size varies; only the lower bound is meaningful here.
        if len(container) < hdr["hdr_size"] + hdr["img_size"]:
            sys.exit("container is shorter than its own header claims")

    if args.pubkey_out:
        raw = pubkey_blob(args.key)
        with open(args.pubkey_out, "wb") as fh:
            fh.write(raw)

    # Self-check: imgtool's own verifier has to accept what we just wrote, and
    # the KEYHASH in the container has to be the hash of the emitted key.
    run_imgtool(["verify", "-k", args.key, args.output])

    want = key_hash_of(args.key)
    if want not in container:
        sys.exit("the container's KEYHASH is not the hash of %s's public key "
                 "(the loader would refuse it)" % args.key)

    print("signed: %s (%u B container, hdr %u, image %u, payload runs at "
          "0x%08x)" % (args.output, len(container), hdr["hdr_size"],
                       hdr["img_size"], load_addr))
    if args.bitstream:
        print("fabric   : upload it with `tools/agm_upload.py <port> bitstream "
              "%s` (the loader verifies it again before it streams it)" % args.output)
    if args.pubkey_out:
        conf = ("CONFIG_BOOT_AGM_SIG_RSA2048_PSS" if is_rsa(
            serialization.load_pem_private_key(open(args.key, "rb").read(),
                                               password=None))
                else "CONFIG_BOOT_AGM_SIG_ECDSA_P256")
        print("public key: %s (%u B) -- build the loader with "
              "-DSPI_BOOT_PUBKEY=%s -D%s=y"
              % (args.pubkey_out, len(raw), args.pubkey_out, conf))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
