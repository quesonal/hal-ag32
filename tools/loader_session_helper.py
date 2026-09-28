#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Reset the board and hold the spi_boot_loader in its console.

Helper for tools/loader_session.sh: opens the serial port *before* the reset
(so the boot banner is not missed), runs tools/openocd_reset_run.sh, and sends
a newline every 10 ms for the given window so that one of them lands inside
the loader's 1.5 s boot-abort window and cancels the boot. The port is then
closed and handed to whoever runs next.

Usage: loader_session_helper.py [port] [window_seconds]
"""

import subprocess
import sys
import time

import serial


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    window = float(sys.argv[2]) if len(sys.argv) > 2 else 2.2
    reset = f"{__file__.rsplit('/', 1)[0]}/openocd_reset_run.sh"

    ser = serial.Serial(port, 115200, timeout=0.01)
    ser.reset_input_buffer()
    proc = subprocess.Popen([reset], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)

    out = b""
    deadline = time.time() + window
    while time.time() < deadline:
        ser.write(b"\r\n")
        out += ser.read(8192)
        time.sleep(0.01)
    proc.wait()
    ser.close()

    sys.stdout.write(out.decode("utf-8", "replace"))
    sys.stdout.write("\n--- loader console (%.1fs window) ---\n" % window)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
