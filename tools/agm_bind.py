#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# agm_bind.py — per-chip binding: the salt, the key it derives, and the tag an
# image has to carry to run on that one chip.
#
# Why a salt
# ----------
# The obvious "derive a key from the chip's unique ID" is not binding: the UID
# is readable by any code that runs on the chip (two public constants unlock
# the flash controller), so an attacker who can run code
# -- or who reads the UID off an unlocked chip -- can derive the same key.
# A per-device *salt*, provisioned next to the firmware and protected by RDP
# is what turns "the UID is a serial number" into "the key is not
# derivable from anything public".
#
# Two consequences worth stating up front, because they are the design:
#   * the salt lives in the on-die flash, so it is only as secret as RDP is
#     strong; the combination is the product, either one alone is not;
#   * every release is per-device (the tag depends on that device's salt), so
#     provisioning needs the device at hand or an online signing service.
#
# Usage
# -----
#   agm_bind.py gen-salt -o salt.bin                 # 16 random bytes
#   agm_bind.py key --uid <32 hex> --salt-file salt.bin
#   agm_bind.py embed --container app.signed.bin --uid <32 hex> \
#                     --salt-file salt.bin -o app.bound.bin
#   agm_bind.py check --container app.bound.bin --uid <32 hex> --salt-file salt.bin
#   agm_bind.py salt-sector --salt-file salt.bin -o sector.bin
#   agm_bind.py uid-from-words 0x36345041 0x12313433 0x36b8d600 0x78010656
#
#   agm_bind.py provision --salt-file salt.bin --uid <32 hex>       # over SWD
#   agm_bind.py provision --salt-file salt.bin --uid-log board.log  # from a log
#
# `key`, the tag and the salt sector are the three things both sides have to
# agree on: the loader recomputes the key from its own UID and the salt it
# reads out of the flash, and the tag over the container it has just verified.
# The KDF is HKDF-SHA256 with a fixed info string, so host and device only have
# to agree on this file -- the known-answer vectors in tools/tests/test_agm_bind.py
# pin both implementations to the same bytes.
#
# Signing and binding are two steps on purpose (`sign_image.py`, then
# `embed`): the signature answers "is this ours", the tag answers "is this
# *this chip's* copy", and only the second one needs the device's salt.

import argparse
import hashlib
import hmac
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import zlib

_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SALT_LEN = 16
UID_LEN = 16
KEY_LEN = 32

# Domain separation: a key derived for one purpose must never be usable for
# another, and a future revision can pick a new string instead of a new
# construction.
INFO_BIND = b"agm-bind-v1"

# The container's TLV area (MCUboot's layout -- see boot_agm_verify.c for the
# walk the device does) and the one type this tool adds. 0x00a0 is inside the
# range MCUboot reserves for vendors (boot/bootutil/include/bootutil/image.h:
# "vendor reserved TLVs at xxA0-xxFF"), so imgtool never emits it and an
# `imgtool image show` prints it as an unknown TLV rather than as one of its
# own -- 0x30, the obvious first pick, is IMAGE_TLV_ENC_RSA2048.
MCUBOOT_HDR_SIZE = 32
MCUBOOT_TLV_INFO_MAGIC = 0x6907
TLV_BIND = 0x00A0
BIND_LEN = 32

# Offset of `ih_ver` in the header: magic, load_addr (4+4), hdr_size,
# protect_tlv_size (2+2), img_size, flags (4+4), then the version's 8 bytes.
MCUBOOT_VER_OFF = 20
MCUBOOT_VER_LEN = 8


def hkdf_sha256(ikm: bytes, salt: bytes, info: bytes, length: int) -> bytes:
    """RFC 5869 HKDF, extract + expand (hashlib/hmac only, no dependencies)."""
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out = b""
    block = b""
    counter = 1

    while len(out) < length:
        block = hmac.new(prk, block + info + bytes([counter]), hashlib.sha256).digest()
        out += block
        counter += 1
    return out[:length]


def bind_key(uid: bytes, salt: bytes) -> bytes:
    """The per-chip binding key: HKDF-SHA256(ikm = UID, salt = the salt).

    The UID goes in as the input key material and the provisioned salt as the
    HKDF salt -- the names are RFC 5869's, and this order is the one to keep:
    the UID is the public half, the salt the secret half.
    """
    if len(uid) != UID_LEN:
        raise ValueError(f"uid must be {UID_LEN} B, got {len(uid)}")
    if len(salt) != SALT_LEN:
        raise ValueError(f"salt must be {SALT_LEN} B, got {len(salt)}")
    return hkdf_sha256(uid, salt, INFO_BIND, KEY_LEN)


def container_body(container: bytes) -> bytes:
    """The `header ‖ image` part of an MCUboot container (no TLV area).

    The binding tag is defined over exactly this, and the reason is a
    self-reference: the tag has to travel *inside* the container (a new TLV),
    so it cannot be computed over the container bytes that include it. The
    header carries the sizes (magic 0x96f3b83d, ih_hdr_size at offset 8,
    ih_img_size at offset 12 -- the layout drivers/misc/boot_agm_verify.c
    parses), so both sides can cut the same slice. The ECDSA signature still
    covers the tag, because it signs over header+image+TLVs.
    """
    if len(container) < 32:
        raise ValueError("container too short to hold an MCUboot header")
    magic, _load, hdr_size, _ptlv, img_size = struct.unpack_from("<IIHHI", container, 0)

    if magic != 0x96F3B83D:
        raise ValueError(f"not an MCUboot container (magic 0x{magic:08x})")
    end = hdr_size + img_size
    if end > len(container):
        raise ValueError(f"header says {end} B, container is {len(container)} B")
    return container[:end]


def version_bytes(container: bytes) -> bytes:
    """The container header's own version field (`ih_ver`, 8 bytes).

    The version goes into the HMAC so a device cannot be moved to another
    release by copying a tag over (the anti-rollback floor is the board-wide
    version story; this keeps the two consistent). It is read *from the
    container* rather than typed in by the operator: a string like "2.0" that
    imgtool pins to 2.0.0 would otherwise produce an image the device refuses,
    for a reason that is invisible in either side's output.
    """
    if len(container) < MCUBOOT_VER_OFF + MCUBOOT_VER_LEN:
        raise ValueError("container too short to hold an MCUboot header")
    return container[MCUBOOT_VER_OFF:MCUBOOT_VER_OFF + MCUBOOT_VER_LEN]


def bind_tag(key: bytes, container: bytes) -> bytes:
    """32-byte tag over (ih_ver ‖ SHA-256(header ‖ image)), keyed per chip.

    The loader recomputes this after it has verified the container against the
    trusted public key, so the tag only has to answer "and is it this chip's
    copy". The digest half is exactly what the signature covers, which is why
    the device does not have to hash the container a second time -- it feeds
    the hash it already has into the same HMAC (boot_agm_bind_tag_digest()).
    """
    if len(key) != KEY_LEN:
        raise ValueError(f"key must be {KEY_LEN} B, got {len(key)}")
    digest = hashlib.sha256(container_body(container)).digest()
    return hmac.new(key, version_bytes(container) + digest, hashlib.sha256).digest()


# --- the BIND TLV ---------------------------------------------------------
#
# The tag has to travel with the image it belongs to, and the only place a
# container has room for it is its TLV area. That works because MCUboot's
# signature covers SHA-256(header ‖ image) and *not* the TLV area: appending a
# TLV after `imgtool sign` leaves the SHA256/KEYHASH/signature TLVs valid, and
# the tag is bound to the signed bytes through the digest it feeds on.


def tlv_area(container: bytes):
    """(offset, total) of the TLV area, validated against the container size."""
    body = len(container_body(container))
    if body + 4 > len(container):
        raise ValueError("container has no TLV area")
    magic, total = struct.unpack_from("<HH", container, body)
    if magic != MCUBOOT_TLV_INFO_MAGIC:
        raise ValueError(f"no TLV info at {body} (magic 0x{magic:04x})")
    if total < 4 or body + total > len(container):
        raise ValueError(f"TLV area is {total} B, container has "
                         f"{len(container) - body} B after the image")
    return body, total


def iter_tlvs(container: bytes):
    """Yield (offset, type, length) for every TLV in the area."""
    body, total = tlv_area(container)
    off = body + 4
    end = body + total

    while off + 4 <= end:
        tlv_type, length = struct.unpack_from("<HH", container, off)
        off += 4
        if off + length > end:
            raise ValueError(f"TLV 0x{tlv_type:04x} runs past the TLV area")
        yield off, tlv_type, length
        off += length


def container_bind(container: bytes):
    """The BIND TLV's value, or None when the container does not carry one."""
    for off, tlv_type, length in iter_tlvs(container):
        if tlv_type == TLV_BIND:
            if length != BIND_LEN:
                raise ValueError(f"BIND TLV has {length} B, expected {BIND_LEN}")
            return container[off:off + length]
    return None


def attach_bind(container: bytes, tag: bytes) -> bytes:
    """Return @a container with a BIND TLV carrying @a tag.

    Adds the TLV at the end of the *TLV area* and grows `it_tlv_tot`; a
    container that already carries one gets it rewritten in place (same length,
    so nothing moves). `imgtool`'s own containers end with the TLV area, so
    this is an append there -- but the insertion point is `it_tlv_tot` rather
    than the file end, so a container with anything after it keeps that.
    """
    if len(tag) != BIND_LEN:
        raise ValueError(f"tag must be {BIND_LEN} B, got {len(tag)}")
    body, total = tlv_area(container)

    for off, tlv_type, length in iter_tlvs(container):
        if tlv_type == TLV_BIND:
            if length != BIND_LEN:
                raise ValueError(f"BIND TLV has {length} B, expected {BIND_LEN}")
            out = bytearray(container)
            out[off:off + length] = tag
            return bytes(out)

    out = bytearray(container[:body + total])
    out += struct.pack("<HH", TLV_BIND, BIND_LEN) + tag
    out += container[body + total:]
    struct.pack_into("<H", out, body + 2, total + 4 + BIND_LEN)
    return bytes(out)


# --- the salt sector (provisioning) ---------------------------------------
#
# One 4 KiB sector in the on-die flash, sector-aligned; in the default layout it
# is the last sector of the A/B chain, directly after the boot record (the two
# together are the 12 KiB metadata block). The format
# is deliberately small and self-checking: magic, version, the 16-byte salt, a
# CRC-32 over those, then 0xff padding.
SALT_SECTOR_LEN = 4096
SALT_SECTOR_MAGIC = 0x424D4741  # "AGMB" little-endian
SALT_SECTOR_VERSION = 1
# Where it goes in the default (on-die A/B) layout. The firmware derives this
# from the layout sizes (boot_agm_priv.h: BOOT_SALT_OFF = loader-size +
# 3*app-size + 2*record-size), so this is the one number the host half and the
# device half have to agree on -- keep it in step with the board overlay's
# layout when app-size/loader-size change. It is *above* everything an image
# write covers, which is the property the driver asserts.
# The sector is *derived* in the firmware (the last sector of the layout's A/B
# chain -- boot_agm_priv.h::BOOT_SALT_OFF), so this tool
# does not carry a default address: it reads the `bind-salt:` line the loader's
# `info` prints (same source as the UID), or takes --salt-addr. Asking the
# device is what keeps the host half and the layout from drifting apart.
SALT_RE = re.compile(r"bind-salt\s*:\s*0x([0-9a-fA-F]+)")


def salt_sector(salt: bytes) -> bytes:
    """Build the 4 KiB sector image from a 16-byte salt."""
    if len(salt) != SALT_LEN:
        raise ValueError(f"salt must be {SALT_LEN} B, got {len(salt)}")
    head = struct.pack("<II", SALT_SECTOR_MAGIC, SALT_SECTOR_VERSION) + salt
    body = head + struct.pack("<I", zlib.crc32(head) & 0xFFFFFFFF)
    return body + b"\xff" * (SALT_SECTOR_LEN - len(body))


def inspect_salt_sector(data: bytes) -> dict:
    """Read a sector back: {'state': 'valid'|'blank'|'invalid', ...}.

    `blank` is what an erased sector reads as (all 0xff, or all 0x00 on a chip
    that reads erased flash as zeroes) -- "nothing provisioned", which is where
    a production line starts. `invalid` is anything else: a sector holding
    somebody else's format, or one whose CRC does not match. Provisioning
    refuses both unless asked with --force, because overwriting a good salt
    silently invalidates every image already signed for that chip.
    """
    if len(data) != SALT_SECTOR_LEN:
        raise ValueError(f"sector must be {SALT_SECTOR_LEN} B, got {len(data)}")
    if data == b"\xff" * SALT_SECTOR_LEN or data == b"\x00" * SALT_SECTOR_LEN:
        return {"state": "blank", "salt": None, "detail": "erased"}

    magic, version = struct.unpack_from("<II", data, 0)
    if magic != SALT_SECTOR_MAGIC:
        return {"state": "invalid", "salt": None,
                "detail": f"magic 0x{magic:08x} is not AGMB"}
    if version != SALT_SECTOR_VERSION:
        return {"state": "invalid", "salt": None,
                "detail": f"layout version {version}, this tool writes "
                          f"{SALT_SECTOR_VERSION}"}
    crc = struct.unpack_from("<I", data, 24)[0]
    if crc != zlib.crc32(data[:24]) & 0xFFFFFFFF:
        return {"state": "invalid", "salt": None, "detail": "CRC mismatch"}
    return {"state": "valid", "salt": data[8:24], "detail": "AGMB v1"}


def uid_from_words(words) -> bytes:
    """The UID as the device reads it: four little-endian words."""
    return b"".join(struct.pack("<I", w) for w in words)


def uid_from_log(text: str) -> bytes:
    """Pull the UID out of a loader console transcript.

    The loader's `info` prints it in the order it read it, as four space
    separated 8-hex-digit words:

        uid      : 41503436 33343112 00d6b836 56060178
    """
    for line in text.splitlines():
        line = line.strip()
        if not line.lower().startswith("uid") or ":" not in line:
            continue
        words = line.split(":", 1)[1].split()
        if len(words) != 4:
            continue
        try:
            return bytes.fromhex("".join(words))
        except ValueError:
            continue
    raise ValueError("no 'uid : ' line with four words in that log")


# --- the device in the loop: provisioning over SWD -------------------------
#
# Writing one sector of the on-die flash is all this needs, and that is the
# path every other write in this repo already takes (openocd with the board's
# support cfg, i.e. the SDK's flash driver, after a `reset init`).
# The *read* side goes through the same session's
# memory read, so it needs no flash driver at all.


def _board_cfg(board: str) -> str:
    return os.environ.get("AGM_OPENOCD_CFG",
                          os.path.join(_REPO, "boards/agm", board,
                                       "support/openocd.cfg"))


def _openocd_bin() -> str:
    exe = os.environ.get("AGM_OPENOCD_CMD", os.path.expanduser(
        "~/AgRV_pio/packages/tool-agrv_openocd/bin/openocd_cmd"))
    if not os.path.exists(exe):
        raise SystemExit(f"FATAL: no openocd at {exe} (set AGM_OPENOCD_CMD); "
                         "the offline subcommands need no probe")
    return exe


def swd_session(cmds, board="agrv2k_407", tries=3):
    """One openocd session: init, reset-init, the given commands, shutdown.

    `reset init` is not decoration -- it is what every flash write in this
    repo is preceded by, and a write without it can leave the AgRV2K flash
    controller wedged -- the ROM bootloader can recover it.
    """
    cmd = [_openocd_bin(), "-f", _board_cfg(board), "-c", "init", "-c", "reset init"]
    for c in cmds:
        cmd += ["-c", c]
    cmd += ["-c", "shutdown"]

    env = dict(os.environ)
    env.setdefault("AGRV_ADAPTER", "cmsis-dap")
    warm = os.path.join(_REPO, "tools/openocd_warmup.py")
    last = ""

    for attempt in range(tries):
        if os.path.exists(warm):
            subprocess.run([sys.executable, warm], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=False)
        proc = subprocess.run(cmd, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if proc.returncode == 0:
            return proc.stdout
        last = proc.stdout
        print(f"    (swd session failed, retry {attempt + 1}/{tries})", file=sys.stderr)
    raise SystemExit("FATAL: openocd session failed:\n" + last)


def swd_read(addr: int, length: int, board="agrv2k_407") -> bytes:
    with tempfile.NamedTemporaryFile(delete=False) as fh:
        path = fh.name
    try:
        swd_session([f"dump_image {path} 0x{addr:08x} {length}"], board=board)
        with open(path, "rb") as fh:
            return fh.read()
    finally:
        os.unlink(path)


def swd_write(addr: int, data: bytes, board="agrv2k_407") -> None:
    with tempfile.NamedTemporaryFile(delete=False) as fh:
        fh.write(data)
        path = fh.name
    try:
        swd_session([f"flash erase_address 0x{addr:08x} 0x{len(data):x}",
                     f"flash write_image {path} 0x{addr:08x} bin"], board=board)
    finally:
        os.unlink(path)


def parse_uid(text: str) -> bytes:
    """32 hex digits, however they happen to be spaced/punctuated."""
    uid = bytes.fromhex("".join(text.split()).replace(":", "").replace("-", ""))
    if len(uid) != UID_LEN:
        raise SystemExit(f"FATAL: a UID is {UID_LEN} B ({UID_LEN * 2} hex "
                         f"digits), got {len(uid)} B")
    return uid


def uid_from_port(port: str, board="agrv2k_407") -> bytes:
    """Reset into the loader's console and read the UID out of its `info`.

    The loader is the authority on this: `agm_boot_unique_id()` is the same
    flex-read the boot path uses, so the value binding derives from is the one
    the chip will actually compute with.
    """
    helper = os.path.join(_REPO, "tools/loader_session_helper.py")
    subprocess.run([sys.executable, helper, port, "2.4"], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    import serial  # only this path needs a UART

    ser = serial.Serial(port, 115200, timeout=0.2)
    text = ""
    try:
        ser.reset_input_buffer()
        ser.write(b"info\r\n")
        deadline = time.time() + 5.0
        while time.time() < deadline:
            text += ser.read(4096).decode("utf-8", "replace")
            if "uid" in text and ":" in text.split("uid", 1)[1][:12]:
                break
    finally:
        ser.close()
    return uid_from_log(text)


def salt_addr_from_log(text: str):
    """The `bind-salt: 0x…` line the loader's `info` prints (None if absent)."""
    m = SALT_RE.search(text)
    return int(m.group(1), 16) if m else None


def salt_addr_from_port(port: str, board="agrv2k_407"):
    """Reset into the loader's console and read the salt address out of `info`."""
    helper = os.path.join(_REPO, "tools/loader_session_helper.py")
    subprocess.run([sys.executable, helper, port, "2.4"], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    import serial

    ser = serial.Serial(port, 115200, timeout=0.2)
    text = ""
    try:
        ser.reset_input_buffer()
        ser.write(b"info\r\n")
        deadline = time.time() + 5.0
        while time.time() < deadline:
            chunk = ser.read(4096)
            if chunk:
                text += chunk.decode("utf-8", "replace")
            elif "bind-salt" in text:
                break
    finally:
        ser.close()
    addr = salt_addr_from_log(text)
    if addr is None:
        raise SystemExit(
            "FATAL: the loader's `info` does not print a `bind-salt:` line --\n"
            "       either this build has no CONFIG_BOOT_AGM_BIND, or the firmware\n"
            "       predates the derived-salt-address change. Pass --salt-addr.")
    return addr


def cmd_provision(args) -> int:
    salt = open(args.salt_file, "rb").read()
    if len(salt) != SALT_LEN:
        raise SystemExit(f"FATAL: the salt file is {len(salt)} B, "
                         f"expected {SALT_LEN}")

    uid = None
    if args.uid:
        uid = parse_uid(args.uid)
    elif args.uid_log:
        uid = uid_from_log(open(args.uid_log).read())
        print_uid(uid, f"from {args.uid_log}")
    elif args.port:
        uid = uid_from_port(args.port, args.board)
        print_uid(uid, f"read from {args.port}")

    # Where the salt goes: the device prints it in `info` (`bind-salt: 0x…`),
    # so the derived layout is the firmware's business instead of a host-side
    # constant somebody has to keep in step.
    addr = args.salt_addr
    if addr is None and args.uid_log:
        addr = salt_addr_from_log(open(args.uid_log).read())
        if addr is not None:
            print(f"salt     : 0x{addr:08x} (from {args.uid_log})")
    if addr is None and args.port:
        addr = salt_addr_from_port(args.port, args.board)
        print(f"salt     : 0x{addr:08x} (read from {args.port})")
    if addr is None:
        raise SystemExit(
            "FATAL: no salt address. The layout derives it, so pass --salt-addr,\n"
            "       or --uid-log/--port and the tool reads the loader's\n"
            "       `bind-salt:` line in the loader info dump.")
    now = None

    if args.read_only and args.no_read:
        raise SystemExit("FATAL: --read-only has nothing to report without the read")
    if not args.no_read:
        now = inspect_salt_sector(swd_read(addr, SALT_SECTOR_LEN, args.board))
        print(f"sector   : 0x{addr:08x} -- {now['state']} ({now['detail']})")
        if args.read_only:
            if now["state"] != "valid":
                raise SystemExit("FATAL: no salt provisioned (--read-only)")
            # What the board itself would compute: the useful thing to
            # compare against a production record, and enough to catch a
            # mixed-up salt file without writing anything.
            print_key(uid, now["salt"], addr, "the salt in the flash")
            print("read-only: nothing written")
            return 0
        if now["state"] == "valid" and now["salt"] == salt:
            print("           already holds this salt; nothing to do")
            print_key(uid, salt, addr, "this salt")
            return 0
        if now["state"] != "blank" and not args.force:
            raise SystemExit(
                "FATAL: 0x%08x already holds something (%s). Overwriting it "
                "invalidates every image bound to this chip so far; pass "
                "--force if that is what you mean."
                % (addr, now["detail"]))

    if args.dry_run:
        print(f"dry run  : would write {SALT_SECTOR_LEN} B to 0x{addr:08x}")
        return 0

    swd_write(addr, salt_sector(salt), args.board)
    back = inspect_salt_sector(swd_read(addr, SALT_SECTOR_LEN, args.board))
    if back["state"] != "valid" or back["salt"] != salt:
        raise SystemExit(f"FATAL: read-back does not match ({back['detail']}); "
                         "do not bind any image to this board yet")
    print(f"wrote    : {SALT_SECTOR_LEN} B at 0x{addr:08x}, read back and verified")

    if print_key(uid, salt, addr, "this salt"):
        print("next     : tools/agm_bind.py embed --container <signed.bin> "
              "--uid <the UID above> --salt-file <this salt> -o <bound.bin>")
    print(f"keep     : {args.salt_file} is the secret half of this chip's key")
    return 0


def print_uid(uid: bytes, where: str) -> None:
    print(f"uid      : {' '.join(uid.hex()[i:i + 8] for i in range(0, 32, 8))}"
          f"  ({where})")


def print_key(uid, salt: bytes, addr: int, what: str) -> bool:
    """The fingerprint of the key UID + @a salt derive, when the UID is known.

    The fingerprint, not the key: it is enough to check that host and device
    agree (the loader prints the same one in `info`) and a hash of a 128-bit
    secret gives nothing away.
    """
    if uid is None:
        print("key      : pass --uid (or --uid-log/--port) to print its fingerprint")
        return False
    key = bind_key(uid, salt)
    print(f"key      : fp {hashlib.sha256(key).digest()[:4].hex()} "
          f"({what}; the loader's derived salt address 0x{addr:08x})")
    return True


def add_uid_args(p, required=True):
    p.add_argument("--uid", help="32 hex digits, as the device reads it",
                   required=required)
    p.add_argument("--salt-file", required=required)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("gen-salt", help="write a fresh 16-byte salt")
    p.add_argument("-o", "--out", required=True)

    p = sub.add_parser("key", help="print the per-chip binding key")
    add_uid_args(p)

    p = sub.add_parser("tag", help="print the tag one image has to carry")
    p.add_argument("--container", required=True, help="signed MCUboot container")
    add_uid_args(p)

    p = sub.add_parser("embed", help="write a bound container (adds the BIND TLV)")
    p.add_argument("--container", required=True, help="signed MCUboot container")
    p.add_argument("-o", "--out", required=True)
    add_uid_args(p)

    p = sub.add_parser("check", help="does this container carry this chip's tag?")
    p.add_argument("--container", required=True)
    add_uid_args(p)

    p = sub.add_parser("salt-sector", help="build the 4 KiB flash image for a salt")
    p.add_argument("--salt-file", required=True)
    p.add_argument("-o", "--out", required=True)

    p = sub.add_parser("uid-from-words",
                       help="turn the four words the device prints into hex")
    p.add_argument("words", nargs=4, type=lambda v: int(v, 0))

    p = sub.add_parser("provision",
                       help="write the salt sector to a board over SWD")
    p.add_argument("--salt-file", required=True)
    p.add_argument("--uid", help="the UID, if it is already known")
    p.add_argument("--uid-log", help="a console transcript to read the UID from")
    p.add_argument("--port", help="reset the board and read the UID from it")
    p.add_argument("--board", default="agrv2k_407")
    p.add_argument("--salt-addr", type=lambda v: int(v, 0), default=None,
                   help="the salt sector, if it is already known (otherwise the "
                        "tool reads the loader's `bind-salt:` line)")
    p.add_argument("--force", action="store_true",
                   help="overwrite a sector that is not empty")
    p.add_argument("--read-only", action="store_true",
                   help="report what the sector holds and stop")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--no-read", action="store_true",
                   help="skip the pre-write read-back")

    args = ap.parse_args(argv)

    if args.cmd == "gen-salt":
        with open(args.out, "wb") as fh:
            fh.write(os.urandom(SALT_LEN))
        print(f"wrote {SALT_LEN} B salt to {args.out} (keep it: it is the secret half)")
        return 0

    if args.cmd == "uid-from-words":
        print(uid_from_words(args.words).hex())
        return 0

    if args.cmd == "provision":
        return cmd_provision(args)

    salt = open(args.salt_file, "rb").read()

    if args.cmd == "salt-sector":
        with open(args.out, "wb") as fh:
            fh.write(salt_sector(salt))
        print(f"wrote {SALT_SECTOR_LEN} B sector image to {args.out} "
              f"(tools/agm_bind.py provision writes it at the address the "
              f"loader prints as `bind-salt:`)")
        return 0

    key = bind_key(parse_uid(args.uid), salt)

    if args.cmd == "key":
        print(key.hex())
        return 0

    container = open(args.container, "rb").read()

    if args.cmd == "tag":
        print(bind_tag(key, container).hex())
        return 0

    if args.cmd == "embed":
        bound = attach_bind(container, bind_tag(key, container))
        with open(args.out, "wb") as fh:
            fh.write(bound)
        print(f"wrote {args.out} ({len(bound)} B, +{len(bound) - len(container)} B "
              f"for the BIND TLV)")
        return 0

    # check
    have = container_bind(container)
    want = bind_tag(key, container)
    if have is None:
        raise SystemExit("FAIL: the container has no BIND TLV (unbound image)")
    if have != want:
        raise SystemExit(f"FAIL: the container is bound to another chip or "
                         f"release (tag {have.hex()} != {want.hex()})")
    print(f"OK: bound to this chip and this release (tag {want.hex()})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
