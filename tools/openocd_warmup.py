#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# openocd_warmup.py — pre-flash warmup for the AgRV2K on-board CMSIS-DAP
# probe (VID=cafe, PID=1001) running FW 2.1.0 on Linux.
#
# Why this exists
# ----------------
# The probe's USB device firmware does NOT respond to GET_DESCRIPTOR(STRING)
# within OpenOCD's hard-coded libusb timeout of 1000ms once it has been
# used at least once. After the first successful openocd session, all
# subsequent invocations time out on the string read and openocd bails
# with "could not read product string ... Operation timed out".
#
# Workaround
# ----------
# Do ONE slow control transfer via pyusb (5s timeout) BEFORE openocd
# starts. After the warmup, the probe's response latency drops back into
# openocd's 1s libusb window and openocd can read its own strings. No
# physical re-plug needed; verified with 3 consecutive invocations of a
# west-flashed firmware image (8192 bytes written + 6828 verified each
# time).
#
# Why pyusb ctrl_transfer instead of usb.util.get_string
# ------------------------------------------------------
# libusb_get_string_descriptor_ascii (used internally by get_string) has a
# fixed 1000ms timeout with no way to override. pyusb's ctrl_transfer
# accepts an explicit `timeout=` kwarg, which is what we need.
#
# Why this script never detaches kernel drivers (root fix)
# -------------------------------------------------------------------
# Earlier versions ran:
#     for intf in (0, 1, 2):
#         if dev.is_kernel_driver_active(intf):
#             dev.detach_kernel_driver(intf)
# That detached cdc_acm from interfaces 1+2 — the UART bridge that shows
# up as /dev/ttyACM0. Nothing re-attached it after openocd shutdown (host
# cdc_acm does not auto-rebind on this uhci_hcd setup), so every west
# flash silently removed /dev/ttyACM0, and repeated sessions made the
# probe look "dead" until a physical replug.
#
# Root cause: the detach was never needed. openocd only claims the
# CMSIS-DAP interface (intf 0, vendor-specific class, no kernel driver
# bound); interfaces 1+2 are the CDC-ACM serial bridge and openocd never
# touches them. EP0 control transfers (this warmup) work without claiming
# or detaching any interface, so cdc_acm stays bound the whole time.
#
# After the detach removal : ~10 consecutive west flash +
# manual openocd sessions left cdc_acm bound throughout — kernel log shows
# zero USB disconnect/reset and /dev/ttyACM0 survives every flash. No
# post-flash rebind is needed; post_flash_rebind.py ("方案 B") is obsolete.
#
# Exit codes
# ----------
# 0  - probe warmed up (or already warm); openocd should succeed
# 1  - probe not found (no device with VID=cafe PID=1001)
# 2  - warmup control transfer failed (probe is wedged beyond recovery)
#
# Usage
# -----
# Invoked by board.cmake as a CMake custom target dependency before the
# openocd flash runner. Not normally run by users directly.

import sys
import time

try:
    import usb
except ImportError:
    print(
        "ERROR: pyusb not installed. Run: pip install pyusb",
        file=sys.stderr,
        flush=True,
    )
    sys.exit(1)

AGRV_PROBE_VID = 0xCAFE
AGRV_PROBE_PID = 0x1001
WARMUP_TIMEOUT_MS = 5000


def main():
    dev = usb.core.find(idVendor=AGRV_PROBE_VID, idProduct=AGRV_PROBE_PID)
    if dev is None:
        print(f"ERROR: probe {AGRV_PROBE_VID:#06x}:{AGRV_PROBE_PID:#06x} not found",
              file=sys.stderr, flush=True)
        sys.exit(1)

    # NOTE: deliberately no detach_kernel_driver() calls here. See the
    # header — detaching cdc_acm (intf 1+2) made /dev/ttyACM0 vanish after
    # every flash; openocd only needs the CMSIS-DAP intf 0, which has no
    # kernel driver bound anyway.

    # Warmup: read language-id and product string descriptors with a long
    # timeout. This is the KEY step — it makes the probe responsive enough
    # that openocd's 1s libusb window can read strings on its own.
    print(f"warming up probe with slow GET_DESCRIPTOR ({WARMUP_TIMEOUT_MS}ms timeout)...",
          flush=True)
    try:
        # string[0] — language IDs (returns 4 bytes: langid LSB, MSB)
        r = dev.ctrl_transfer(0x80, 0x06, 0x0300, 0x0000, 4, timeout=WARMUP_TIMEOUT_MS)
        langid = int.from_bytes(bytes(r), "little") & 0xFFFF
        print(f"  string[0] OK: langid=0x{langid:04x}", flush=True)
        time.sleep(0.1)
        # string[2] — product string (UTF-16LE, returns descriptor bytes)
        r = dev.ctrl_transfer(0x80, 0x06, 0x0302, langid, 64, timeout=WARMUP_TIMEOUT_MS)
        # Strip the leading 2-byte length + 1-byte type; decode UTF-16LE.
        chars = bytes(r[2:]).decode("utf-16-le", errors="replace")
        print(f"  string[2] OK: product={chars!r}", flush=True)
    except usb.core.USBError as e:
        print(f"ERROR: warmup failed: {e}", file=sys.stderr, flush=True)
        sys.exit(2)

    # cdc_acm stayed bound to intf 1+2 the whole time: /dev/ttyACM0 is
    # preserved across flash + openocd shutdown. Nothing to rebind.


if __name__ == "__main__":
    main()
