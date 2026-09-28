#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# rom_opt.py — read or write the AgRV2K flash *option* area over the ROM
# bootloader (BOOT0 high, UART0), i.e. the path that needs no probe.
#
# Why this exists
# ---------------
# The FPGA configuration address lives in the option area, and the SDK's tools
# can only write it through the AP/SWD path (`tools/agm_oo.sh bitstream`,
# `oo ... options_write FPGA`). That matters for the recovery ladder: after an
# option erase the chip reads as *read-protected* to the system bus and the ROM
# refuses writes, so the usual answer is "get the probe out".
#
# The ROM itself can write it, and `agrv32flash` cannot be used to: its -O
# erases the options and then *resets* the device, and after that reset the ROM
# sees the erased options as protected and NACKs everything (it also refuses
# `-O` together with `-w`). The vendor's own "custom programmer" reference
# (uart_master.c::uart_write_options) does it the way this tool does: erase the
# options and write the image **in one session, with no reset in between**.
#: the write lands,
# verified by an independent SWD readback of 0x81000000.
#
# Two things to know before using it: writing the option area triggers a *full
# chip erase* (the images are gone afterwards — re-flash code and logic), and
# the chip then stops answering the ROM protocol for ~10 s while it erases, so
# the init loop here is deliberately patient.
#
# Usage
#   tools/rom_opt.py show                          # decode + print
#   tools/rom_opt.py save opt.bin                  # baseline to restore from
#   tools/rom_opt.py restore opt.bin               # erase + write + verify
#   tools/rom_opt.py set-fpga 0x800e7000           # uncompressed form
#   tools/rom_opt.py set-fpga --compressed 0x800f4b00 --algo 0x800f4000

import argparse
import struct
import sys
import time

import serial

ACK = 0x79
NACK = 0x1F

# The option area as the ROM sees it: 128 B of "option RAM", of which the
# vendor's tools write the first 72 (18 words) - see gen_batch's layout and
# uart_master.c::uart_write_options().
OPT_ADDR = 0x81000000
OPT_LEN = 72

# Word offsets inside that image (32-bit words).
OPT_WORD_RDP_USER = 0
OPT_WORD_CFG_ADDR = 12   # uncompressed: config address, then its complement
OPT_WORD_CPLX_CFG = 14   # compressed: config address, then its complement
OPT_WORD_CPLX_ALGO = 16  # compressed: decompression algorithm, then complement
RDP_WORD = 0x5AA5        # the "unprotected" half word the vendor writes


def half_ok(lo, hi):
    """The vendor's half-word convention: value + complement == 0xFF."""
    return (lo + hi) & 0xFF == 0xFF


def dword_ok(words, i):
    """32-bit field convention: value + complement == 0xFFFFFFFF."""
    return words[i] != 0xFFFFFFFF and (words[i] + words[i + 1]) == 0xFFFFFFFF


def decode_option_image(img):
    """Decode the fields the boot path cares about (vendor conventions)."""
    words = list(struct.unpack("<%dI" % (len(img) // 4), img[:len(img) // 4 * 4]))
    out = {
        "rdp_unprotected": half_ok(words[0] & 0xFF, (words[0] >> 8) & 0xFF),
        "compressed": False,
        "config_addr": None,
        "algo_addr": None,
    }
    if dword_ok(words, OPT_WORD_CFG_ADDR):
        out["config_addr"] = words[OPT_WORD_CFG_ADDR]
    if dword_ok(words, OPT_WORD_CPLX_CFG):
        out["compressed"] = True
        out["config_addr"] = words[OPT_WORD_CPLX_CFG]
        out["algo_addr"] = words[OPT_WORD_CPLX_ALGO] if dword_ok(words, OPT_WORD_CPLX_ALGO) else None
    return out


def set_config_addr(img, addr, compressed=False, algo=None):
    """Point the option image at a config (and, when compressed, its algorithm).

    Everything else in the image is preserved: a board's OSC/user fields are
    not ours to invent, so the callers read-modify-write rather than build a
    fresh image.
    """
    words = list(struct.unpack("<%dI" % (len(img) // 4), img[:len(img) // 4 * 4]))

    if not compressed:
        words[OPT_WORD_CFG_ADDR] = addr
        words[OPT_WORD_CFG_ADDR + 1] = (~addr) & 0xFFFFFFFF
        for i in (OPT_WORD_CPLX_CFG, OPT_WORD_CPLX_CFG + 1,
                  OPT_WORD_CPLX_ALGO, OPT_WORD_CPLX_ALGO + 1):
            words[i] = 0xFFFFFFFF
    else:
        words[OPT_WORD_CFG_ADDR] = 0xFFFFFFFF
        words[OPT_WORD_CFG_ADDR + 1] = 0xFFFFFFFF
        words[OPT_WORD_CPLX_CFG] = addr
        words[OPT_WORD_CPLX_CFG + 1] = (~addr) & 0xFFFFFFFF
        if algo is not None:
            words[OPT_WORD_CPLX_ALGO] = algo
            words[OPT_WORD_CPLX_ALGO + 1] = (~algo) & 0xFFFFFFFF
    # The RDP half word (the image's first two bytes, little endian) is the
    # "unprotected" marker the vendor's tools write; keep the other half of
    # that word (and the rest of the image) as it was.
    words[0] = (words[0] & 0xFFFF0000) | RDP_WORD
    return struct.pack("<%dI" % len(words), *words)


class Rom:
    """The vendor's ROM-bootloader protocol, as uart_master.c speaks it."""

    def __init__(self, port, baud=57600, timeout=1.0):
        self.s = serial.Serial(port, baud, parity=serial.PARITY_EVEN,
                               bytesize=8, stopbits=1, timeout=timeout)

    def init(self, tries=30, on_try=None):
        """0x7F until the ROM answers 0x79.

        The chip is deaf for ~10 s while the full-chip erase that a previous
        option write triggered is running, so this is patient on purpose.

        Note what this deliberately does *not* do: flush the input buffer
        between probes. The first 0x7F puts the ROM into its command state, and
        its ACK can still be in flight when a naive loop gives up and flushes
        -- the next 0x7F is then a command to that ROM and comes back as a
        NACK. Exactly that produced an alternating
        "no answer" / "0x1f" pattern that never converged. Drain and scan
        instead, so a late ACK still ends the loop.
        """
        pending = b""
        for i in range(tries):
            self.s.write(b"\x7f")
            deadline = time.time() + 0.5
            while time.time() < deadline:
                chunk = self.s.read(64)
                if not chunk:
                    continue
                pending += chunk
                if ACK in pending:
                    return True
            if on_try:
                on_try(i, pending[-1:] or b"")
            pending = b""
        return False

    def _ack(self, what, timeout=None):
        r = self.s.read(1)
        if not r:
            raise IOError("%s: no answer" % what)
        if r[0] == NACK:
            raise IOError("%s: NACK" % what)
        if r[0] != ACK:
            raise IOError("%s: unexpected 0x%02x" % (what, r[0]))

    def cmd(self, c):
        self.s.write(bytes([c, (~c) & 0xFF]))
        self._ack("command 0x%02x" % c)

    def addr(self, a):
        b = bytes([(a >> 24) & 0xFF, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF])
        self.s.write(b + bytes([b[0] ^ b[1] ^ b[2] ^ b[3]]))
        self._ack("address 0x%08x" % a)

    def write(self, a, data, legacy=False):
        """0x31: address, then N (= len-1), the bytes and the XOR checksum."""
        if len(data) == 0 or len(data) > 256:
            raise ValueError("one write frame carries 1..256 bytes")
        self.cmd(0x31)
        self.addr(a)
        if legacy:
            self.cmd(0x00)
        n = len(data) - 1
        xor = n
        for byte in data:
            xor ^= byte
        self.s.write(bytes([n]) + data + bytes([xor & 0xFF]))
        self._ack("write 0x%08x+%d" % (a, len(data)))

    def read(self, a, length, legacy=False):
        """0x11: address, then N and its complement, then N+1 bytes back."""
        self.cmd(0x11)
        self.addr(a)
        if legacy:
            self.cmd(0x00)
        n = length - 1
        self.s.write(bytes([n, (~n) & 0xFF]))
        self._ack("read request 0x%08x+%d" % (a, length))
        out = b""
        while len(out) < length:
            chunk = self.s.read(length - len(out))
            if not chunk:
                raise IOError("read 0x%08x: short answer (%d/%d)" % (a, len(out), length))
            out += chunk
        return out

    def option_erase(self):
        """0xA3. Triggers a full chip erase."""
        self.cmd(0xA3)

    def reset_target(self):
        """0xA2: reboot the device, ending the session cleanly.

        This is what the vendor's master does at the end of a flow, and it
        matters for the *next* run: the ROM stays in its command state after
        the port closes, so a following 0x7F probe is read as a command and
        NACKed, and the reason a run only worked right after a
        board reset.
        """
        self.cmd(0xA2)
        try:
            self.s.read(1)   # the ROM answers this one twice
        except serial.SerialException:
            pass

    def close(self):
        self.s.close()


def _connect(args, quiet=False, tries=None):
    r = Rom(args.port, args.baud)
    if not r.init(tries=tries or args.init_tries,
                  on_try=None if quiet else
                  (lambda i, resp: print("  probe %d: %s"
                                         % (i + 1, "no answer" if not resp
                                            else "0x%02x" % resp[0]), file=sys.stderr))):
        r.close()
        sys.exit("the ROM bootloader did not answer 0x7F. Check BOOT0 is high, "
                 "then reset the board: the ROM stays in its command state "
                 "after a session, and a reset is what gets it back to "
                 "listening (power-cycle, or tools/openocd_reset_run.sh).")
    return r


def _read_options(r, args):
    return r.read(OPT_ADDR, args.length, legacy=args.legacy)


def cmd_show(args):
    r = _connect(args)
    img = _read_options(r, args)
    r.reset_target()
    r.close()
    fields = decode_option_image(img)
    print("option image (%d B from 0x%08x):" % (len(img), OPT_ADDR))
    print("  read protection : %s" % ("off (RDP half word A5 5A)"
                                      if fields["rdp_unprotected"] else "side unknown"))
    print("  form            : %s" % ("compressed"
                                      if fields["compressed"] else "uncompressed"))
    print("  config address  : %s" % ("0x%08x" % fields["config_addr"]
                                      if fields["config_addr"] is not None
                                      else "not valid"))
    if fields["compressed"]:
        print("  algorithm addr  : %s" % ("0x%08x" % fields["algo_addr"]
                                          if fields["algo_addr"] is not None
                                          else "not valid"))
    print("  raw             : %s" % img.hex())
    return 0


def cmd_save(args):
    r = _connect(args)
    img = _read_options(r, args)
    r.reset_target()
    r.close()
    open(args.file, "wb").write(img)
    print("saved %d B from 0x%08x to %s" % (len(img), OPT_ADDR, args.file))
    return 0


def _erase_and_write(args, image):
    """The verified recipe: one session, erase then write, no reset between."""
    r = _connect(args)
    print("erasing option bytes (0xA3) ... this triggers a full chip erase")
    r.option_erase()
    print("writing %d B to 0x%08x ..." % (len(image), OPT_ADDR))
    r.write(OPT_ADDR, image, legacy=args.legacy)
    # End the session the way the vendor's master does. Without this the ROM
    # stays in its command state after the port closes, and the verify's 0x7F
    # probe is read as a command and NACKed: the write had landed
    # (SWD agreed) while the tool's own verify could not get back in.
    try:
        r.reset_target()
    except (IOError, serial.SerialException):
        pass
    # The verify is a fresh session on purpose: an immediate read-back can still
    # show the pre-write content, and the chip is deaf for a while
    # anyway while the full chip erase runs.
    r.close()
    # The vendor multiplies its timeout for this write because it triggers the
    # full chip erase; the chip is deaf for tens of seconds afterwards, which a
    # normal init budget (30 x 0.5 s) does not cover.
    time.sleep(3.0)
    r = _connect(args, tries=max(args.init_tries * 2, 60))
    back = _read_options(r, args)
    r.reset_target()
    r.close()
    if back == image:
        print("verified: 0x%08x holds the image that was written" % OPT_ADDR)
        return 0
    print("MISMATCH: read back %s" % back.hex(), file=sys.stderr)
    print("          wrote    %s" % image.hex(), file=sys.stderr)
    return 1


def cmd_restore(args):
    image = open(args.file, "rb").read()
    if len(image) < OPT_LEN:
        sys.exit("%s is %d B, need at least %d" % (args.file, len(image), OPT_LEN))
    print("NOTE: writing the option area erases the whole chip: re-flash the "
          "firmware and the bitstream afterwards.")
    return _erase_and_write(args, image[:args.length])


def cmd_set_fpga(args):
    r = _connect(args)
    img = _read_options(r, args)
    r.reset_target()
    r.close()
    new = set_config_addr(img, args.addr, compressed=args.compressed, algo=args.algo)
    if new == img:
        print("nothing to do: 0x%08x already holds this configuration" % OPT_ADDR)
        return 0
    fields = decode_option_image(new)
    print("NOTE: writing the option area erases the whole chip: re-flash the "
          "firmware and the bitstream afterwards.")
    print("target: %s config at 0x%08x%s"
          % ("compressed" if args.compressed else "uncompressed", args.addr,
             ", algorithm at 0x%08x" % args.algo if args.compressed else ""))
    return _erase_and_write(args, new)


def main(argv):
    ap = argparse.ArgumentParser(
        description="read/write the AgRV2K option area over the ROM bootloader "
                    "(BOOT0 high)")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=57600)
    ap.add_argument("--length", type=lambda s: int(s, 0), default=OPT_LEN,
                    help="bytes of the option area to touch (default 72, the "
                         "vendor tools' image size)")
    ap.add_argument("--init-tries", type=int, default=30,
                    help="0x7F probes before giving up (0.2 s apart); a chip "
                         "that just erased its options is deaf for ~10 s")
    ap.add_argument("--legacy", action="store_true",
                    help="send the extra mode byte after each address; ROMs "
                         "older than version 0x20 need it, ours (0x20) do not")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("show", help="read and decode the option image")
    p = sub.add_parser("save", help="read the option image into a file")
    p.add_argument("file")
    p = sub.add_parser("restore", help="erase the options, then write a saved image")
    p.add_argument("file")
    p = sub.add_parser("set-fpga", help="point the FPGA config address at an address")
    p.add_argument("addr", type=lambda s: int(s, 0))
    p.add_argument("--compressed", action="store_true",
                   help="the compressed form: config in <addr>, algorithm in --algo")
    p.add_argument("--algo", type=lambda s: int(s, 0))

    args = ap.parse_args(argv)
    if args.cmd == "show":
        return cmd_show(args)
    if args.cmd == "save":
        return cmd_save(args)
    if args.cmd == "restore":
        return cmd_restore(args)
    if args.cmd == "set-fpga":
        if args.compressed and args.algo is None:
            sys.exit("--compressed needs --algo <address> (the ROM runs it)")
        return cmd_set_fpga(args)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
