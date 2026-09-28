#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# agm_upload.py — push an image into samples/spi_boot_loader's stores over the
# console UART.
#
# The loader's `upload <a|b|bitstream|slot>` command switches its console into a
# binary phase; this tool drives it (`slot` = the on-die flash DFU: the image is
# programmed straight into the application slot of the internal flash):
#
#   header (16 B): [0xA5][cmd][target][reserved:1][offset:4 LE][len:4 LE][crc32:4 LE]
#   payload      : len bytes, CRC32 (zlib / Zephyr crc32_ieee) over them
#
#   cmd 0x01 DATA   : payload = image bytes for that offset
#   cmd 0x02 FINISH : payload = total_len:4 | crc32:4 | load:4 | entry:4
#
# Every frame is answered with 0x79 (ACK) or 0x1F + error code. FINISH makes
# the loader re-read the whole store from flash and only then publish it: an
# application store becomes TRIAL+active (so the attempt-counted rollback
# takes over), the bitstream target is staged and applied to the on-die
# reservation immediately.
#
# Usage:
#   tools/agm_upload.py <port> <a|b|bitstream|slot> <image.bin> [--load 0x... --entry 0x...]
#
# The load/entry addresses only matter for application stores (they are what
# the loader jumps to); they default to the on-die slot of the default
# (on-die A/B) layout, 0x8007c000.

import argparse
import sys
import time
import zlib

try:
    import serial
except ImportError:
    print("ERROR: pyserial not installed (pip install pyserial)", file=sys.stderr)
    sys.exit(1)

MAGIC = 0xA5
ACK = 0x79
NAK = 0x1F
CMD_DATA = 0x01
CMD_FINISH = 0x02
TARGETS = {"a": 0, "b": 1, "bitstream": 2, "slot": 3}
CHUNK = 1024
# Keep in step with UP_ERR_* in drivers/misc/boot_agm.c: the loader separated
# "the verifier refused this image" from "the flash write failed" because the
# two look nothing alike in a log, and used to share code 3.
ERR = {
    1: "bad argument/frame",
    2: "payload CRC mismatch",
    3: "flash write/verify failed",
    4: "image rejected (bad signature/digest, wrong slot, or no trusted key)",
    5: "image is older than the one installed (anti-rollback)",
    6: "no authorization for this command (production build)",
}

# What the loader prints when a production-locked build refuses a state change
# (drivers/misc/boot_agm.c). Both are printed *before* the protocol bytes
# the tool is waiting for, so they are also the fastest way to tell an operator
# that this path cannot work at all -- and what can .
LOCKED_REFUSALS = (b"refusing to open an upload", b"refusing to publish")
LOCKED_HINT = (
    "this board is production-locked: the console upload phase and agrv32flash "
    "cannot publish\n"
    "  (an upload needs one grant to open the session and another to publish, and "
    "neither path can be handed the second one mid-flight).\n"
    "  Use:  tools/smp_cli.py <port> upload <image> <file> --authorize <key.pem>\n"
    "  A locked build with CONFIG_BOOT_AGM_SMP=n has no in-band path at all -- "
    "recover over SWD or the BOOT0/ROM bootloader."
)


def locked_refusal(data: bytes) -> bool:
    return any(marker in data for marker in LOCKED_REFUSALS)


def wait_for(ser, marker: bytes, timeout: float, fail_markers: tuple = ()) -> bytes:
    """Read until `marker` shows up (returns everything read).

    `fail_markers` end the wait early with a RuntimeError: used for the loader's
    production-lock refusals, which mean the command will never be answered --
    waiting the full timeout for a `READY` that is not coming is a 10 s stall
    and a message that does not say what to do instead.
    """
    data = b""
    end = time.time() + timeout
    while time.time() < end:
        data += ser.read(4096)
        if marker in data:
            return data
        if any(m in data for m in fail_markers):
            raise RuntimeError(LOCKED_HINT + "\n\nloader said:\n"
                               + data.decode("utf-8", "replace").strip())
        time.sleep(0.02)
    raise TimeoutError(f"no {marker!r} within {timeout}s; got:\n" + data.decode("utf-8", "replace"))


def frame(cmd: int, target: int, offset: int, payload: bytes) -> bytes:
    # 16-byte header: the loader reads [magic][cmd][target][reserved] then
    # three 32-bit little-endian fields. Sending 15 bytes shifted every field
    # by one, which is what made the first frame come back as NAK/bad-argument
    # (the length field landed on the CRC bytes).
    return (bytes([MAGIC, cmd, target, 0])
            + offset.to_bytes(4, "little")
            + len(payload).to_bytes(4, "little")
            + (zlib.crc32(payload) & 0xFFFFFFFF).to_bytes(4, "little")
            + payload)


def expect_ack(ser, what: str, timeout: float = 30.0, verbose: bool = False,
               grace: float = 0.05) -> None:
    """Wait for the frame's reply and return, or raise on a NAK.

    The loader prints its verdict on the *same* UART as the protocol bytes (it
    is where "image verified ..." and "refusing an older image ..." come from),
    so "the first 0x79 is the ACK" is wrong: the letter 'y' is 0x79, and a
    refusal that mentions "layout" used to read as success
    ). Collect the whole burst instead and decide:

      * a NAK (0x1F + code) anywhere in it wins -- 0x1F cannot appear in the
        loader's printable text, so it is always the protocol's;
      * otherwise an ACK (0x79) anywhere is the reply;
      * neither, and the frames would have to be re-sent (caller retries).

    @a grace is how long to keep looking *after* the first ACK byte: the verdict
    that may follow it (the FINISH refusal) has to be caught, but paying that
    wait on every frame would add ~0.2 s per kilobyte of upload -- the port's
    read timeout used to be the de-facto window. The port timeout is therefore
    shortened to @a grace once an ACK is seen, and restored on the way out.
    DATA frames (a bare ACK, no verdict) pass the short default; FINISH passes
    a longer one, since that is the only frame the loader answers with text
    first.
    """
    end = time.time() + timeout
    seen = b""
    acked = False
    saved_timeout = ser.timeout
    try:
        while time.time() < end:
            # Once the ACK is in, only the verdict may still be coming: read
            # with the (short) grace window instead of the port's full timeout.
            ser.timeout = grace if acked else saved_timeout
            b = ser.read(1)
            if not b:
                if acked:
                    break  # nothing followed the ACK: the reply is complete
                continue
            seen += b
            # A NAK is unambiguous (0x1F never appears in the loader's
            # printable text), so take it and its code byte and stop -- reading
            # further would eat the reply to whatever the caller sends next.
            # (That is also why there is no second NAK scan after the loop: a
            # byte that made it into `seen` was already classified here.)
            if b[0] == NAK:
                code_b = ser.read(1)
                code = code_b[0] if code_b else -1
                text = seen.decode("utf-8", "replace").replace("\r", "\n").strip()
                if text:
                    print("loader said:\n" + text, file=sys.stderr)
                # Code 6 is the authorization gate ("no authorization"): the bytes may
                # even have reached the store, but this path can never publish
                # in a locked build, so say what can .
                hint = ("\n" + LOCKED_HINT) if code == 6 else ""
                raise RuntimeError(
                    f"{what}: loader NAK (code {code}: {ERR.get(code, 'unknown')})" + hint)
            if b[0] == ACK and not acked:
                acked = True
            if verbose:
                sys.stdout.write(b.decode("utf-8", "replace"))
                sys.stdout.flush()
    finally:
        ser.timeout = saved_timeout

    if acked:
        return
    text = seen.decode("utf-8", "replace").replace("\r", "\n").strip()
    if text:
        print("loader said:\n" + text, file=sys.stderr)
    raise TimeoutError(f"{what}: no ACK within {timeout}s"
                       + (f" (loader sent {len(seen)} bytes of console text)" if seen else ""))


def send_strict(ser, frm: bytes, what: str) -> None:
    """One frame, then exactly one byte: the ACK. Anything else is reported --
    used to tell a real ACK from a 0x79 inside echoed console bytes."""
    ser.write(frm)
    b = ser.read(1)
    if not b:
        raise TimeoutError(f"{what}: no reply")
    if b[0] != ACK:
        rest = ser.read(4096)
        raise RuntimeError(f"{what}: expected ACK, got 0x{b[0]:02x} (then "
                           f"{rest[:60]!r})")


def send_with_retry(ser, frm: bytes, what: str, retries: int = 4,
                    verbose: bool = False) -> None:
    """Send a frame, resending it when the loader reports a frame CRC error.

    The console link drops a byte now and then (the loader answers NAK/2, the
    frame CRC, without touching the store), and both DATA and FINISH frames
    are idempotent in that case: a rejected frame has changed nothing, so
    sending the same bytes again is safe.
    """
    for attempt in range(1, retries + 1):
        ser.write(frm)
        try:
            # FINISH is the frame the loader answers with its verdict text
            # first, so it gets a longer grace window than the bare-ACK DATA
            # frames (see expect_ack()).
            finish = "FINISH" in what

            expect_ack(ser, what, timeout=120.0 if finish else 30.0,
                       verbose=verbose, grace=0.2 if finish else 0.05)
            if attempt > 1:
                print(f"  {what}: ok on attempt {attempt}")
            return
        except RuntimeError as e:
            if "code 2" not in str(e) or attempt == retries:
                raise
            print(f"  {what}: frame CRC NAK, resending (attempt {attempt + 1})")
            time.sleep(0.2)


def main() -> int:
    ap = argparse.ArgumentParser(description="upload an image to spi_boot_loader")
    ap.add_argument("port")
    ap.add_argument("target", choices=sorted(TARGETS))
    ap.add_argument("image")
    # The application slot of the default (on-die) layout: loader 96 KiB,
    # record, two 200 KiB sides, then the slot. See
    # samples/spi_boot_loader/boards/agrv2k_407.overlay; a signed image
    # carries the same number in its header and the loader refuses a mismatch.
    ap.add_argument("--load", type=lambda v: int(v, 0), default=0x8007c000)
    ap.add_argument("--entry", type=lambda v: int(v, 0), default=0x8007c000)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--verbose", action="store_true", help="echo the loader console")
    ap.add_argument("--strict", action="store_true",
                    help="require exactly one ACK byte per frame (diagnostic)")
    args = ap.parse_args()

    with open(args.image, "rb") as f:
        image = f.read()
    if not image:
        print("ERROR: image is empty", file=sys.stderr)
        return 1

    target = TARGETS[args.target]
    crc = zlib.crc32(image) & 0xFFFFFFFF
    print(f"uploading {len(image)} B to store '{args.target}' (crc 0x{crc:08x}) "
          f"load 0x{args.load:08x} entry 0x{args.entry:08x}")

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    ser.reset_input_buffer()
    # Enter the console deterministically, then the binary phase.
    ser.write(b"\r\n")
    time.sleep(0.2)
    ser.reset_input_buffer()
    for ch in f"upload {args.target}\r\n".encode():
        ser.write(bytes([ch]))
        time.sleep(0.004)
    wait_for(ser, b"READY", 10.0, fail_markers=LOCKED_REFUSALS)
    ser.reset_input_buffer()

    t0 = time.time()
    for off in range(0, len(image), CHUNK):
        chunk = image[off:off + CHUNK]
        frm = frame(CMD_DATA, target, off, chunk)
        if args.strict:
            send_strict(ser, frm, f"chunk @0x{off:x}")
        else:
            send_with_retry(ser, frm, f"chunk @0x{off:x}", verbose=args.verbose)
    finish = (len(image).to_bytes(4, "little")
              + crc.to_bytes(4, "little")
              + args.load.to_bytes(4, "little")
              + args.entry.to_bytes(4, "little"))
    send_with_retry(ser, frame(CMD_FINISH, target, 0, finish), "FINISH",
                    verbose=args.verbose)
    print(f"done in {time.time() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
