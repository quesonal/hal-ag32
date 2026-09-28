#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# smp_cli.py — a minimal, dependency-light SMP (mcumgr) client for dev board work.
#
# Why another one: `smpmgr` (smpclient) is the full-featured host, and the
# legacy Go `mcumgr` CLI needs a Go toolchain that this dev board does not have.
# This script is a *second, independent* implementation of the wire format
# (the base64 lines behind the 0x0609/0x0414 headers, the SMP header and the
# CBOR body), so a device bug in the framing shows up against two different
# hosts instead of one. It deliberately implements only what the dev board needs:
#
#   smp_cli.py <port> state                 # image state (read, op 0)
#   smp_cli.py <port> upload <image> <file> # image upload (write, op 1)
#   smp_cli.py <port> upload <image> <file> --authorize <key.pem>
#   smp_cli.py <port> erase <image>         # image erase (write, op 5)
#   smp_cli.py <port> nonce                 # the loader's own group 0x40, cmd 0
#   smp_cli.py <port> authorize <key.pem> <erase|publish> [args-hex]
#
# `image` is the bootloader driver's target: 0/1 = the external stores, 2 = the
# on-die application slot, 3 = bitstream staging (see
# samples/verify_flow).
#
# `authorize` is what a production-locked build (CONFIG_BOOT_AGM_LOCK_PRODUCTION,
# ) wants before it publishes an upload or
# erases a target: it fetches a nonce, signs SHA-256(nonce || cmd || args_len ||
# args) with the key that signs the images, and sends that back. The grant is
# device-wide and one shot, so the state-changing command has to follow before
# anything else spends it.
#
# A locked build gates the session *and* the publish, so `upload --authorize`
# spends both grants for the host: one ERASE before the first chunk (that
# first write erases the target) and one PUBLISH before the chunk whose reply
# publishes the image.
#
# The transport only needs pyserial and cbor2; everything else is stdlib.

import binascii
import struct
import sys
import time

import cbor2

try:
    import serial
except ImportError:  # pragma: no cover
    print("ERROR: pyserial not installed (pip install pyserial)", file=sys.stderr)
    raise SystemExit(2)

SMP_OP_READ = 0
SMP_OP_WRITE = 2
SMP_GROUP_IMAGE = 1
# The loader's own group (drivers/misc/boot_agm_smp.c): mcumgr reserves ids
# below 64 for the standard groups, so this one sits at 0x40.
SMP_GROUP_BOOT_AUTH = 0x40

IMG_STATE = 0
IMG_UPLOAD = 1
IMG_ERASE = 5

BOOT_AUTH_NONCE = 0
BOOT_AUTH_AUTHORIZE = 1

# AGM_BOOT_AUTH_CMD_* (include/zephyr/drivers/misc/boot_agm.h).
AUTH_CMD = {"erase": 1, "publish": 2}

CHUNK = 400  # payload bytes per request: keeps the base64 line under 128 chars


def crc16_itu_t(data: bytes) -> int:
    """CRC16 used by Zephyr's serial SMP framing (crc16_itu_t, init 0x0000)."""
    crc = 0x0000
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode_packet(payload: bytes, line_length: int = 127) -> bytes:
    """[0x06 0x09][len BE][payload][crc16 BE] -> base64 lines, each + '\\n'.

    One line carries at most `line_length` bytes: the 2-byte delimiter, the
    base64 text and the newline (Zephyr's transport uses 128-byte lines and
    feeds each one to mcumgr_serial_process_frag()). 0x0609 starts a packet,
    0x0414 continues it.
    """
    frame = struct.pack(">H", len(payload) + 2) + payload + struct.pack(
        ">H", crc16_itu_t(payload)
    )
    b64 = binascii.b2a_base64(frame, newline=False).decode()
    per_line = (line_length - 3) & ~3  # 4 base64 chars per 3 bytes
    lines = [b64[i : i + per_line] for i in range(0, len(b64), per_line)]
    out = b""
    for index, piece in enumerate(lines):
        out += (b"\x06\x09" if index == 0 else b"\x04\x14") + piece.encode() + b"\n"
    return out


def decode_packet(lines: list) -> bytes:
    """Inverse of encode_packet(): returns the raw SMP packet."""
    data = b""
    for line in lines:
        head, body = line[:2], line[2:].strip()
        if head != b"\x06\x09" and head != b"\x04\x14":
            raise SystemExit(f"bad delimiter {head.hex()} in {line!r}")
        data += binascii.a2b_base64(body)
    if len(data) < 2:
        raise SystemExit(f"short frame {data.hex()}")
    (length,) = struct.unpack(">H", data[:2])
    payload, crc = data[2 : 2 + length - 2], data[length:]
    if len(payload) + 2 != length or len(crc) != 2:
        raise SystemExit(f"length mismatch: {data.hex()}")
    if struct.unpack(">H", crc)[0] != crc16_itu_t(payload):
        raise SystemExit("CRC mismatch")
    return payload


def smp_request(op: int, group: int, command: int, seq: int, body: bytes) -> bytes:
    """SMP v2 header (little endian bitfields: op:3 | version:2) + CBOR body."""
    return struct.pack(">BBHHBB", (1 << 3) | op, 0, len(body), group, seq, command) + body


def parse_smp_reply(packet: bytes):
    op, flags, length, group, seq, command = struct.unpack(">BBHHBB", packet[:8])
    body = packet[8 : 8 + length]
    # A handler that answers with no body at all means success (the mcumgr
    # convention); an error always carries {"rc": ...}.
    return (op, group, command, seq), (cbor2.loads(body) if body else {})


class Smp:
    def __init__(self, port: str, baud: int = 115200, timeout: float = 5.0):
        self.ser = serial.Serial(port, baud, timeout=timeout)
        self.seq = 0

    def close(self):
        self.ser.close()

    def request(self, op, group, command, body: bytes, timeout: float = 5.0):
        packet = smp_request(op, group, command, self.seq, body)
        self.ser.reset_input_buffer()
        self.ser.write(encode_packet(packet))
        deadline = time.time() + timeout
        lines = []
        while time.time() < deadline:
            line = self.ser.readline()
            if not line:
                continue
            if line[:2] in (b"\x06\x09", b"\x04\x14"):
                lines.append(line)
                try:
                    head = parse_smp_reply(decode_packet(lines))[0]
                except SystemExit:
                    continue  # still a fragment: 0x0414 lines carry the rest
                if head[3] == self.seq:
                    break
        if not lines:
            raise SystemExit("timeout waiting for the reply")
        self.seq = (self.seq + 1) & 0xFF
        return parse_smp_reply(decode_packet(lines))


def do_state(smp: Smp) -> int:
    header, body = smp.request(SMP_OP_READ, SMP_GROUP_IMAGE, IMG_STATE, b"\xbf\xff")
    print(header, body)
    for image in body.get("images", []):
        print(
            "  image %s: version=%s bootable=%s active=%s confirmed=%s"
            % (
                image.get("image"),
                image.get("version"),
                image.get("bootable"),
                image.get("active"),
                image.get("confirmed"),
            )
        )
    return 0


def do_upload(smp: Smp, image: int, path: str, key_path: str = "") -> int:
    data = open(path, "rb").read()
    off = 0
    first = True

    if key_path:
        # The locked profile opens the session on an ERASE grant; the publish
        # needs its own (one grant = one command).
        if do_authorize(smp, key_path, "erase"):
            return 1
    while True:
        chunk = data[off : off + CHUNK]
        # The last chunk's reply is the one that publishes (mcumgr has no
        # separate "finish" request), so its grant has to be in place first.
        if key_path and off + len(chunk) >= len(data):
            if do_authorize(smp, key_path, "publish"):
                return 1
        req = {"off": off, "data": chunk}
        if first:
            req["image"] = image
            req["len"] = len(data)
        _, reply = smp.request(
            SMP_OP_WRITE, SMP_GROUP_IMAGE, IMG_UPLOAD, cbor2.dumps(req), timeout=30.0
        )
        if reply.get("rc", 0) != 0:
            print("upload failed:", reply, file=sys.stderr)
            return 1
        off = reply.get("off", off + len(chunk))
        print(f"\r{off}/{len(data)} bytes", end="", flush=True)
        first = False
        if off >= len(data):
            break
    print("\ndone")
    return 0


def do_erase(smp: Smp, image: int) -> int:
    _, reply = smp.request(
        SMP_OP_WRITE, SMP_GROUP_IMAGE, IMG_ERASE, cbor2.dumps({"slot": image}), timeout=60.0
    )
    print(reply)
    return 0 if reply.get("rc", 0) == 0 else 1


def get_nonce(smp: Smp) -> bytes:
    _, reply = smp.request(SMP_OP_READ, SMP_GROUP_BOOT_AUTH, BOOT_AUTH_NONCE, b"\xbf\xff")
    nonce = reply.get("nonce")
    if reply.get("rc", 0) != 0 or not nonce:
        raise SystemExit(f"the loader did not hand out a nonce: {reply}")
    return bytes(nonce)


def auth_payload(nonce: bytes, cmd: int, args: bytes = b"") -> bytes:
    """The byte string the device rebuilds from the request and hashes.

    `nonce[16] || cmd[1] || args_len[2] (LE) || args` -- exactly what
    agm_boot_authorize() documents and what the loader's SHA-256 runs over
    (include/zephyr/drivers/misc/boot_agm.h).
    """
    return bytes(nonce) + bytes([cmd]) + struct.pack("<H", len(args)) + bytes(args)


def sign_auth_payload(key, payload: bytes) -> bytes:
    """SHA-256 `payload` and sign *that*, in DER.

    `Prehashed` is the whole point: the device hashes the payload itself and
    hands the 32-byte digest to its ECDSA verifier, so signing the payload
    without it would hash the bytes twice and every command would be refused.
    """
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec, utils

    digest = hashes.Hash(hashes.SHA256())
    digest.update(payload)
    return key.sign(digest.finalize(), ec.ECDSA(utils.Prehashed(hashes.SHA256())))


def do_nonce(smp: Smp) -> int:
    print("nonce", get_nonce(smp).hex())
    return 0


def do_authorize(smp: Smp, key_path: str, cmd_name: str, args_hex: str = "") -> int:
    """Authorize one state-changing command with `key.pem`."""
    if cmd_name not in AUTH_CMD:
        raise SystemExit(f"unknown command {cmd_name!r}; known: {', '.join(AUTH_CMD)}")

    try:
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ec
    except ImportError:  # pragma: no cover
        raise SystemExit("ERROR: `cryptography` is needed to sign (pip install cryptography)")

    with open(key_path, "rb") as fh:
        key = serialization.load_pem_private_key(fh.read(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey):
        raise SystemExit(f"{key_path} is not an ECDSA key (the command verifier is P-256)")

    # The byte string the device reconstructs and hashes: the nonce it just
    # handed out, the command byte, the argument length little endian, then the
    # arguments (empty here: the grant names a command, not a target).
    args = bytes.fromhex(args_hex) if args_hex else b""
    nonce = get_nonce(smp)
    payload = auth_payload(nonce, AUTH_CMD[cmd_name], args)
    signature = sign_auth_payload(key, payload)

    body = cbor2.dumps(
        {
            "nonce": nonce,
            "cmd": AUTH_CMD[cmd_name],
            "args": args,
            "sig": signature,
        }
    )
    _, reply = smp.request(SMP_OP_WRITE, SMP_GROUP_BOOT_AUTH, BOOT_AUTH_AUTHORIZE, body, timeout=30.0)
    rc = reply.get("rc", 0)
    if rc == 0:
        print(f"authorized {cmd_name}")
        return 0
    print(f"authorize failed: {reply}", file=sys.stderr)
    return 1


def main(argv: list) -> int:
    if len(argv) < 3:
        print(__doc__)
        return 2

    port, action = argv[1], argv[2]
    smp = Smp(port)
    try:
        if action == "state":
            return do_state(smp)
        if action == "upload" and len(argv) >= 5:
            key = ""
            rest = argv[5:]

            if rest:
                if len(rest) == 2 and rest[0] == "--authorize":
                    key = rest[1]
                else:
                    print(__doc__)
                    return 2
            return do_upload(smp, int(argv[3]), argv[4], key)
        if action == "erase" and len(argv) == 4:
            return do_erase(smp, int(argv[3]))
        if action == "nonce":
            return do_nonce(smp)
        if action == "authorize" and len(argv) >= 5:
            return do_authorize(smp, argv[3], argv[4], argv[5] if len(argv) > 5 else "")
        print(__doc__)
        return 2
    finally:
        smp.close()


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
