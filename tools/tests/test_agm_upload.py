# SPDX-License-Identifier: Apache-2.0
"""Unit tests for tools/agm_upload.py -- the console upload protocol's host side.

The 16-byte header and the FINISH payload are a wire contract with
drivers/misc/boot_agm.c (up_rd32() reads the little-endian fields straight out
of the frame), so they are pinned as golden bytes here rather than left to the
first dev board run that happens to fail.
"""

import importlib.util
import pathlib
import sys
import time
import zlib

import pytest

TOOLS = pathlib.Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


agm_upload = load("agm_upload")


def test_data_frame_layout():
    payload = b"\x01\x02\x03\x04"
    frm = agm_upload.frame(agm_upload.CMD_DATA, 0, 0x1234, payload)

    assert frm[0] == 0xA5, "magic"
    assert frm[1] == 0x01, "DATA"
    assert frm[2] == 0, "target A is wire index 0"
    assert frm[3] == 0, "reserved"
    assert frm[4:8] == (0x1234).to_bytes(4, "little"), "offset is little-endian"
    assert frm[8:12] == len(payload).to_bytes(4, "little"), "length"
    assert frm[12:16] == (zlib.crc32(payload) & 0xFFFFFFFF).to_bytes(4, "little")
    assert frm[16:] == payload


@pytest.mark.parametrize("target,name", [(0, "a"), (1, "b"), (2, "bitstream"), (3, "slot")])
def test_target_numbering(target, name):
    """The wire numbering is the console's own (a, b, bitstream, slot) and does
    *not* match the driver's enum order -- the mapping is in the driver's
    up_wire_target(), and swapping the two breaks "upload b" silently."""
    frm = agm_upload.frame(agm_upload.CMD_DATA, target, 0, b"\x00")

    assert frm[2] == target, name


def test_finish_frame_payload():
    """FINISH carries the whole-image length, its CRC and the load/entry pair;
    the driver reads exactly four little-endian words out of it."""
    total = 0x1234
    crc = zlib.crc32(b"image") & 0xFFFFFFFF
    payload = (
        total.to_bytes(4, "little")
        + crc.to_bytes(4, "little")
        + (0x80030000).to_bytes(4, "little")
        + (0x80030000).to_bytes(4, "little")
    )
    frm = agm_upload.frame(agm_upload.CMD_FINISH, 0, 0, payload)

    assert frm[1] == 0x02
    assert frm[8:12] == (16).to_bytes(4, "little"), "FINISH is exactly 16 bytes"
    assert frm[16:20] == total.to_bytes(4, "little")
    assert frm[20:24] == crc.to_bytes(4, "little")


class FakeSerial:
    """Minimal serial stand-in: records what was written and hands back the
    replies the test queues."""

    def __init__(self, replies):
        self.replies = list(replies)
        self.written = []
        # expect_ack() shortens this to its grace window once an ACK is in and
        # restores it afterwards (the port's own timeout used to be the
        # de-facto window, which cost a full read timeout per frame).
        self.timeout = 0.2
        self.timeouts_seen = []

    def write(self, data):
        self.written.append(data)

    def read(self, size=1):
        self.timeouts_seen.append(self.timeout)
        if not self.replies:
            return b""
        out = self.replies[0][:size]
        self.replies[0] = self.replies[0][size:]
        if not self.replies[0]:
            self.replies.pop(0)
        return out

    @property
    def in_waiting(self):
        return len(self.replies[0]) if self.replies else 0


def test_resend_once_on_frame_crc_nack():
    """A frame-level CRC error (NAK 0x1F 0x02) is the one failure the tool
    retries: the loader stays in the upload phase and resends are expected.
    Anything else (a flash error) aborts -- pretending otherwise would hide a
    half-written store."""
    ser = FakeSerial([b"\x1f\x02", b"\x79"])
    frm = agm_upload.frame(agm_upload.CMD_DATA, 0, 0, b"\x00")

    agm_upload.send_with_retry(ser, frm, "data", retries=4)
    assert len(ser.written) == 2, "the frame was sent again"


def test_no_resend_on_flash_error():
    ser = FakeSerial([b"\x1f\x03"])
    frm = agm_upload.frame(agm_upload.CMD_DATA, 0, 0, b"\x00")

    with pytest.raises(RuntimeError):
        agm_upload.send_with_retry(ser, frm, "data", retries=4)
    assert len(ser.written) == 1, "a flash error is not retried"


def test_expect_ack_returns_on_a_bare_ack():
    ser = FakeSerial([b"\x79"])

    agm_upload.expect_ack(ser, "data", timeout=1.0)
    assert ser.timeout == 0.2, "the port timeout is restored"


def test_expect_ack_sees_the_verdict_behind_the_ack():
    """The bug this rule exists for: the loader prints its verdict on the same
    UART, and 'y' is 0x79 -- so an ACK byte inside that text must not be taken
    as success while a refusal is still on its way."""
    ser = FakeSerial([b"loader: refusing ... this layout\r\n\x1f\x04"])

    with pytest.raises(RuntimeError, match=r"NAK \(code 4"):
        agm_upload.expect_ack(ser, "FINISH", timeout=1.0, grace=0.2)


def test_expect_ack_reports_text_without_an_answer():
    ser = FakeSerial([b"loader: something\r\n"])

    with pytest.raises(TimeoutError):
        agm_upload.expect_ack(ser, "data", timeout=0.05)


def test_expect_ack_reads_the_tail_with_the_grace_window():
    """Once the ACK is in, the port timeout is the grace window -- not the
    0.2 s the port was opened with, which is what made every DATA frame pay a
    full read timeout ."""
    ser = FakeSerial([b"\x79"])

    agm_upload.expect_ack(ser, "data", timeout=1.0, grace=0.01)
    assert 0.01 in ser.timeouts_seen, ser.timeouts_seen
    assert ser.timeout == 0.2


def test_locked_refusal_fails_fast_with_a_pointer():
    """A production-locked build answers the console `upload` command with a
    refusal instead of READY -- and that means this path can never work, so the
    tool says what can instead of waiting the whole READY timeout."""
    ser = FakeSerial([
        b"loader: refusing to open an upload to a: no grant armed for command 0x01 "
        b"(a fresh nonce voids an unused grant)\r\n"
    ])

    start = time.monotonic()
    with pytest.raises(RuntimeError) as excinfo:
        agm_upload.wait_for(ser, b"READY", 10.0, fail_markers=agm_upload.LOCKED_REFUSALS)

    assert time.monotonic() - start < 1.0, "the marker ends the wait, not the timeout"
    assert "production-locked" in str(excinfo.value)
    assert "smp_cli.py" in str(excinfo.value)


def test_unauthorized_nak_points_at_the_smp_upload():
    """The FINISH refusal (NAK code 6) gets the same pointer: the loader is
    locking the door, not reporting a broken image."""
    ser = FakeSerial([b"\x1f\x06"])
    frm = agm_upload.frame(agm_upload.CMD_FINISH, 0, 0, bytes(16))

    with pytest.raises(RuntimeError) as excinfo:
        agm_upload.send_with_retry(ser, frm, "FINISH", retries=1)

    assert "code 6" in str(excinfo.value)
    assert "smp_cli.py" in str(excinfo.value)
