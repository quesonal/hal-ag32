#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# probe_reset_target.py — pulse the target's nRESET through the on-board
# CMSIS-DAP probe (VID=cafe, PID=1001) without starting openocd.
#
# Why this exists
# ---------------
# The AgRV2K latches BOOT0/BOOT1 at "powering on or restarting" (4th
# SYSCLK rising edge), and the BOOT0 strap is what selects the
# ROM UART bootloader that `agrv32flash` / `west flash --runner agrv32flash`
# talk to. openocd's `reset run` is a *soft* system reset (the SDK cfg
# declares no srst — `adapter assert srst` answers "adapter has no srst
# signal"), and it does NOT re-latch the strap:, seven
# `reset run` attempts left the loader answering nothing.
#
# CMSIS-DAP's DAP_ResetTarget (0x0A) does assert the target's nRESET, and
# that *does* re-latch: with the BOOT0 jumper fitted, one pulse is enough for
# `agrv32flash -r` to reach the loader — that is also how
# the UART write path was brought up).
#
# Usage
# -----
#   tools/probe_reset_target.py [--vid 0xCAFE] [--pid 0x1001]
#
# Exit codes
# ----------
# 0  - reset executed (DAP answered OK)
# 1  - probe not found
# 2  - transfer failed / DAP reported an error

import argparse
import sys

try:
    import usb.core
    import usb.util
except ImportError:
    print("ERROR: pyusb not installed (pip install pyusb)", file=sys.stderr)
    sys.exit(1)

DAP_RESET_TARGET = 0x0A
DAP_OK = 0x00
PACKET_SIZE = 64
TIMEOUT_MS = 2000


def find_dap_bulk(dev):
    """Return (out_ep, in_ep) of the CMSIS-DAP v2 vendor interface."""
    for intf in dev.get_active_configuration():
        if intf.bInterfaceClass != 0xFF:
            continue
        out_ep = in_ep = None
        for ep in intf:
            if (ep.bmAttributes & 0x03) != 0x02:      # bulk transfer type
                continue
            if ep.bEndpointAddress & 0x80:            # IN endpoint
                in_ep = ep.bEndpointAddress
            else:
                out_ep = ep.bEndpointAddress
        if out_ep is not None and in_ep is not None:
            return intf.bInterfaceNumber, out_ep, in_ep
    return None, None, None


def main():
    ap = argparse.ArgumentParser(
        description="Pulse the target nRESET through the on-board CMSIS-DAP "
                    "probe (DAP_ResetTarget), so the BOOT0/BOOT1 strap is "
                    "re-latched and the ROM UART bootloader comes up.")
    ap.add_argument("--vid", type=lambda v: int(v, 0), default=0xCAFE)
    ap.add_argument("--pid", type=lambda v: int(v, 0), default=0x1001)
    args = ap.parse_args()

    dev = usb.core.find(idVendor=args.vid, idProduct=args.pid)
    if dev is None:
        print(f"ERROR: probe {args.vid:#06x}:{args.pid:#06x} not found",
              file=sys.stderr)
        return 1

    # No detach_kernel_driver(): interfaces 1+2 are the CDC-ACM console
    # bridge (/dev/ttyACM0) and must stay bound. See openocd_warmup.py.
    intf_num, out_ep, in_ep = find_dap_bulk(dev)
    if out_ep is None:
        print("ERROR: no CMSIS-DAP v2 bulk interface on the probe",
              file=sys.stderr)
        return 2

    usb.util.claim_interface(dev, intf_num)
    try:
        dev.write(out_ep,
                  bytes([DAP_RESET_TARGET]) + b"\x00" * (PACKET_SIZE - 1),
                  timeout=TIMEOUT_MS)
        resp = bytes(dev.read(in_ep, PACKET_SIZE, timeout=TIMEOUT_MS))
    except usb.core.USBError as e:
        print(f"ERROR: DAP_ResetTarget failed: {e}", file=sys.stderr)
        return 2
    finally:
        usb.util.release_interface(dev, intf_num)

    cmd, status = resp[0], resp[1]
    if cmd != DAP_RESET_TARGET or status != DAP_OK:
        print(f"ERROR: DAP_ResetTarget answered cmd={cmd:#04x} "
              f"status={status:#04x}", file=sys.stderr)
        return 2

    print("nRESET pulsed (DAP_ResetTarget OK)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
