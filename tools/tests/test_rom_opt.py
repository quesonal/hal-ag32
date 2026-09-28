# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/rom_opt.py -- the ROM-bootloader path to the option area.

Two things are worth pinning without a board: the **wire framing** (command +
complement, big-endian address + XOR, and the N/data/checksum byte frame), which
is what the vendor's own master sends and what this tool has to send; and the
**option image's field conventions** (the RDP half word, and the address +
complement pairs at words 12/13 vs 14..17), which decide whether the chip loads
a fabric at all after a recovery write.
"""

import importlib.util
import pathlib
import struct
import sys

import pytest

TOOLS = pathlib.Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


rom_opt = load("rom_opt")


class FakeSerial:
    """Records what the tool writes and hands back canned replies."""

    def __init__(self, replies):
        self.written = b""
        self.replies = bytearray(replies)

    def write(self, data):
        self.written += data

    def read(self, n=1):
        out = bytes(self.replies[:n])
        del self.replies[:n]
        return out

    def reset_input_buffer(self):
        pass

    def close(self):
        pass


def make_rom(monkeypatch, replies):
    fake = FakeSerial(replies)
    monkeypatch.setattr(rom_opt.serial, "Serial", lambda *a, **k: fake)
    return rom_opt.Rom("fake"), fake


def test_a_write_frame_is_what_the_vendor_master_sends(monkeypatch):
    """0x31, address (big endian + XOR), N = len-1, data, XOR over N and data."""
    rom, fake = make_rom(monkeypatch, bytes([rom_opt.ACK] * 3))
    rom.write(0x81000000, b"\x01\x02\x03")

    addr = bytes([0x81, 0x00, 0x00, 0x00])
    addr += bytes([addr[0] ^ addr[1] ^ addr[2] ^ addr[3]])
    payload = b"\x01\x02\x03"
    n = len(payload) - 1
    checksum = n
    for byte in payload:
        checksum ^= byte
    assert fake.written == (bytes([0x31, (~0x31) & 0xFF]) + addr
                            + bytes([n]) + payload + bytes([checksum]))


def test_a_read_request_is_the_short_form(monkeypatch):
    """0x11: address, then N and its complement; the bytes follow the ACK."""
    # One ACK for the command, one for the address, one for the length pair.
    rom, fake = make_rom(monkeypatch, bytes([rom_opt.ACK] * 3) + b"\xaa\xbb")
    assert rom.read(0x81000000, 2) == b"\xaa\xbb"

    addr = bytes([0x81, 0x00, 0x00, 0x00])
    addr += bytes([addr[0] ^ addr[1] ^ addr[2] ^ addr[3]])
    n = 1
    assert fake.written == bytes([0x11, (~0x11) & 0xFF]) + addr + bytes([n, (~n) & 0xFF])


def test_option_erase_is_a_command_plus_complement(monkeypatch):
    rom, fake = make_rom(monkeypatch, bytes([rom_opt.ACK]))
    rom.option_erase()
    assert fake.written == bytes([0xA3, (~0xA3) & 0xFF])


def test_a_session_ends_with_the_roms_reset(monkeypatch):
    """0xA2 (+ complement) and its two ACKs.

    Without this the ROM stays in its command state after the port closes and
    the next run's 0x7F probe is NACKed .
    """
    rom, fake = make_rom(monkeypatch, bytes([rom_opt.ACK] * 2))
    rom.reset_target()
    assert fake.written == bytes([0xA2, (~0xA2) & 0xFF])


def test_a_nack_is_an_error_with_a_readable_message(monkeypatch):
    rom, _ = make_rom(monkeypatch, bytes([rom_opt.NACK]))
    with pytest.raises(IOError, match="NACK"):
        rom.option_erase()


# The baseline image of the dev board (read back over SWD before the dev board
# test). Regenerate on a board with `tools/rom_opt.py save`.
BOARD_OPT_IMAGE = bytes.fromhex(
    "a55a"        # RDP half word: little endian A5 5A, i.e. unprotected
    + "ff" * 30   # user/data fields, then the vendor's 24 bytes of filler
    + "ffff"      # misc
    + "57a8"      # OSC -- a board field this tool never invents
    + "ff" * 12
    + "00700e80"  # word 12: config address 0x800e7000, little endian
    + "ff8ff17f"  # word 13: its complement
    + "ff" * 16)  # words 14..17: the compressed pair, empty here


def test_decodes_the_uncompressed_board_image():
    fields = rom_opt.decode_option_image(BOARD_OPT_IMAGE)
    assert fields["rdp_unprotected"] is True
    assert fields["compressed"] is False
    assert fields["config_addr"] == 0x800E7000


def test_setting_an_uncompressed_address_keeps_everything_else():
    new = rom_opt.set_config_addr(BOARD_OPT_IMAGE, 0x800E7000)
    assert new == BOARD_OPT_IMAGE, "same address has to be a no-op"

    moved = rom_opt.set_config_addr(BOARD_OPT_IMAGE, 0x800F4000)
    words = struct.unpack("<18I", moved)
    assert words[12] == 0x800F4000
    assert words[13] == (~0x800F4000) & 0xFFFFFFFF
    assert words[14:18] == (0xFFFFFFFF,) * 4, "the compressed fields stay empty"
    # Everything outside the address field is preserved, byte for byte.
    assert moved[:48] == BOARD_OPT_IMAGE[:48]
    assert moved[56:] == BOARD_OPT_IMAGE[56:]
    assert rom_opt.decode_option_image(moved)["config_addr"] == 0x800F4000


def test_setting_the_compressed_pair_clears_the_plain_field():
    new = rom_opt.set_config_addr(BOARD_OPT_IMAGE, 0x800F4B00,
                                  compressed=True, algo=0x800F4000)
    words = struct.unpack("<18I", new)
    assert words[12:14] == (0xFFFFFFFF, 0xFFFFFFFF)
    assert words[14] == 0x800F4B00 and words[15] == (~0x800F4B00) & 0xFFFFFFFF
    assert words[16] == 0x800F4000 and words[17] == (~0x800F4000) & 0xFFFFFFFF

    fields = rom_opt.decode_option_image(new)
    assert fields["compressed"] is True
    assert fields["config_addr"] == 0x800F4B00
    assert fields["algo_addr"] == 0x800F4000


def test_the_rdp_half_word_is_restored_as_the_vendor_writes_it():
    """A recovery write must leave the chip unprotected, or the next boot is
    read-protected and nothing reads back."""
    zeroed = bytes(72)
    new = rom_opt.set_config_addr(zeroed, 0x800E7000)
    assert new[:2] == b"\xa5\x5a"
    assert rom_opt.decode_option_image(new)["rdp_unprotected"] is True
