#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# usb_forensic_decode.py — Decode the raw word dump produced by
# tools/usb_forensic_dump.sh (openocd `mdw` output lines:
#   "0xADDR: w0 w1 ... w7"
# and optional "pc (/32): 0x..." lines) into human-readable
# USB0 register + udc_agm dQH/dTD state.
#
# Usage:
#   python3 tools/usb_forensic_decode.py <raw_dump_file>
#
# Struct layouts mirror drivers/usb/udc/udc_agm.h and the AgRV SDK
# usb.h (USB_DQH / USB_DTD), verified identical.

import re
import sys

DQH_BASE = 0x20002000
DQH_STRIDE = 0x40
DTD_BASE = 0x20002200
DTD_STRIDE = 0x20
USB_BASE = 0x41001000

REG_NAMES = {
    0x41001140: "USBCMD", 0x41001144: "USBSTS", 0x41001148: "USBINTR",
    0x4100114C: "FRINDEX", 0x41001154: "DEVICEADDR",
    0x41001158: "ENDPOINTLISTADDR", 0x41001180: "CONFIGFLAG",
    0x41001184: "PORTSC", 0x410011A4: "OTGSC", 0x410011A8: "USBMODE",
    0x410011AC: "ENDPTSETUPSTAT", 0x410011B0: "ENDPTPRIME",
    0x410011B4: "ENDPTFLUSH", 0x410011B8: "ENDPTSTATUS",
    0x410011BC: "ENDPTCOMPLETE",
}
CTRL_BASE = 0x410011C0

USBSTS_BITS = [
    (0, "UI"), (1, "UEI"), (2, "PCI"), (3, "FRI"), (4, "SEI"),
    (5, "AAI"), (6, "URI"), (7, "SRI"), (8, "SLI"), (12, "HCH"),
    (13, "RCL"), (14, "PS"), (15, "AS"), (16, "NAKI"),
]
PORTSC_BITS = [
    (0, "CCS"), (1, "CSC"), (2, "PE"), (3, "PEC"), (6, "FPR"),
    (7, "SUSP"), (8, "PR"), (12, "PP"),
]


def bits_str(v, bits):
    return " ".join(n for b, n in bits if v & (1 << b))


def ep_bits(v, label):
    """ENDPTSETUPSTAT/PRIME/FLUSH/STATUS/COMPLETE: low16=OUT ep n, high16=IN ep n."""
    out = []
    for n in range(4):
        if v & (1 << n):
            out.append("%sOUT%d" % (label, n))
        if v & (1 << (16 + n)):
            out.append("%sIN%d" % (label, n))
    return " ".join(out) if out else "0"


def dtd_status(s):
    tb = (s >> 16) & 0x7FFF
    bits = []
    if s & 0x80: bits.append("ACTIVE")
    if s & 0x40: bits.append("HALTED")
    if s & 0x20: bits.append("DATA_ERR")
    if s & 0x08: bits.append("TRAN_ERR")
    if s & 0x8000: bits.append("IOC")
    return "%s total_bytes=%d" % ("|".join(bits) if bits else "idle", tb)


def parse(path):
    mem = {}
    pc = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"\s*0x([0-9a-fA-F]+):\s*(.*)", line)
            if m:
                addr = int(m.group(1), 16)
                for i, w in enumerate(re.findall(r"[0-9a-fA-F]{8}", m.group(2))):
                    mem[addr + i * 4] = int(w, 16)
                continue
            m = re.match(r"\s*pc \(/32\):\s*0x([0-9a-fA-F]+)", line)
            if m:
                pc = int(m.group(1), 16)
    return mem, pc


def decode(mem, pc):
    if pc is not None:
        print("PC      = 0x%08x%s" % (pc, " (idle WFI)" if pc & 0xFFFFFFF8 == 0x80003778 else ""))
    print()
    print("== SoC clock gates ==")
    for a, n in ((0x03000050, "AHB_RESET"), (0x03000070, "AHB_CLKENABLE"),
                 (0x03000060, "APB_CLKENABLE")):
        print("  %-16s @0x%08x = 0x%08x" % (n, a, mem.get(a, 0)))
    print()
    print("== USB0 registers (base 0x%08x) ==" % USB_BASE)
    for a in sorted(REG_NAMES):
        v = mem.get(a, 0)
        n = REG_NAMES[a]
        if n == "USBCMD":
            print("  USBCMD      = 0x%08x  RS=%d RST=%d FS=%d SUTW=%d ATDTW=%d" %
                  (v, v & 1, (v >> 1) & 1, (v >> 2) & 3, (v >> 13) & 1, (v >> 14) & 1))
        elif n == "USBSTS":
            print("  USBSTS      = 0x%08x  %s" % (v, bits_str(v, USBSTS_BITS)))
        elif n == "PORTSC":
            print("  PORTSC      = 0x%08x  %s speed=%d" %
                  (v, bits_str(v, PORTSC_BITS), (v >> 26) & 3))
        elif n == "OTGSC":
            print("  OTGSC       = 0x%08x  ID=%d(%s)" % (v, (v >> 8) & 1,
                  "A-device/host" if (v >> 8) & 1 == 0 else "B-device/device"))
        elif n == "USBMODE":
            print("  USBMODE     = 0x%08x  CM=%d(%s) SLOM=%d" %
                  (v, v & 3, ("device" if v & 3 == 2 else "host" if v & 3 == 3 else "idle"),
                   (v >> 3) & 1))
        elif n == "DEVICEADDR":
            print("  DEVICEADDR  = 0x%08x  addr=%d" % (v, (v >> 25) & 0x7F))
        elif n in ("ENDPTSETUPSTAT", "ENDPTPRIME", "ENDPTFLUSH",
                   "ENDPTSTATUS", "ENDPTCOMPLETE"):
            print("  %-10s = 0x%08x  %s" % (n, v, ep_bits(v, n)))
        elif n == "ENDPOINTLISTADDR":
            print("  ENDPOINTLISTADDR = 0x%08x" % v)
        else:
            print("  %-16s = 0x%08x" % (n, v))
    for i in range(4):
        v = mem.get(CTRL_BASE + 4 * i, 0)
        print("  ENDPTCTRL[%d]  = 0x%08x  RX:e=%d type=%d | TX:e=%d type=%d" %
              (i, v, (v >> 7) & 1, (v >> 2) & 3, (v >> 23) & 1, (v >> 18) & 3))

    print()
    print("== dQH pool @0x%08x (8 x 0x40) ==" % DQH_BASE)
    epnames = ["EP0-OUT", "EP0-IN", "EP1-OUT", "EP1-IN",
               "EP2-OUT", "EP2-IN", "EP3-OUT", "EP3-IN"]
    for i in range(8):
        q = DQH_BASE + i * DQH_STRIDE
        info = mem.get(q + 0x00, 0)
        cur = mem.get(q + 0x04, 0)
        onxt = mem.get(q + 0x08, 0)
        ost = mem.get(q + 0x0C, 0)
        obuf = mem.get(q + 0x10, 0)
        oexp = (mem.get(q + 0x24, 0) & 0xFFFF)
        sb0 = mem.get(q + 0x28, 0)
        sb1 = mem.get(q + 0x2C, 0)
        print("  dQH[%d] %-7s @0x%05x: mps=%d zlt=%d int_on_setup=%d current=0x%08x" %
              (i, epnames[i], q, (info >> 16) & 0x7FF, (info >> 29) & 1,
               (info >> 15) & 1, cur))
        print("           overlay: next=0x%08x status=0x%08x [%s] buf=0x%08x expected=%d" %
              (onxt, ost, dtd_status(ost), obuf, oexp))
        if i < 2:
            sb = (sb1 << 32) | sb0
            print("           setup_buffer=0x%016x (%s)" %
                  (sb, " ".join("%02x" % ((sb >> (8 * k)) & 0xFF) for k in range(8))))

    print()
    print("== dTD pool @0x%08x (8 x 0x20) ==" % DTD_BASE)
    for i in range(8):
        d = DTD_BASE + i * DTD_STRIDE
        nxt = mem.get(d + 0x00, 0)
        st = mem.get(d + 0x04, 0)
        b0 = mem.get(d + 0x08, 0)
        exp = mem.get(d + 0x1C, 0) & 0xFFFF
        print("  dTD[%d] %-7s @0x%05x: next=0x%08x status=0x%08x [%s] buf=0x%08x expected=%d" %
              (i, epnames[i], d, nxt, st, dtd_status(st), b0, exp))


def main():
    if len(sys.argv) != 2:
        print("Usage: %s <raw_dump_file>" % sys.argv[0])
        return 1
    mem, pc = parse(sys.argv[1])
    if not mem:
        print("no mdw lines parsed from %s" % sys.argv[1])
        return 1
    decode(mem, pc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
