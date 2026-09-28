#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# boot_timing.py — timestamp the console around a `reset run` and print the
# offsets of the lines that matter, so boot-path costs can be quoted instead
# of guessed.
#
# Why it exists
# -------------
# The signing measurement needed "how long does the boot path
# spend before it can print", which no device-side clock can answer: the
# signed-fabric verifier runs in PRE_KERNEL_1, *before* the kernel timer
# exists and before z_cstart() switches the CPU off HSI. The only honest
# anchor is the reset itself plus the host's view of when the console
# answered.
#
# Two things make that anchor usable:
#   * the reset goes through a *running* openocd's telnet port (one command,
#     milliseconds) instead of spawning openocd per run, whose ~1 s startup
#     would swamp the measurement;
#   * the reader is opened before the reset, so the first byte is not lost --
#     the same reason tools/test_uart_capture.sh starts `cat` first.
#
# Start the debugger yourself (leave it running) and then call this:
#
#   openocd -f boards/agm/agrv2k/shared/support/agrv2k-minimal.cfg -c init &
#   tools/boot_timing.py --runs 5 --window 4
#
# Notes
#   * the minimal cfg must be able to run `reset run`: on OpenOCD 0.12 the
#     `mmw` in its examine-end event is only defined after
#     `[find mem_helper.tcl]` is sourced (see the cfg's own comment);
#   * lines printed before the reset (a still-running image) are shown with
#     negative offsets and are excluded from the marks;
#   * `--window` has to cover the slowest boot: a signed fabric adds seconds
#     (the verifier runs at HSI).

import argparse
import socket
import threading
import time

import serial

# Console lines that mark a phase of samples/spi_boot_loader's boot. The
# measurement is only as good as these anchors, so they are listed here
# rather than buried in the loop below.
MARKS = {
    "fabric": b"fabric came from",          # after the pre-kernel FCB work
    "window": b"booting in",                # start of the 1.5 s console window
    "verify": b"image verified",            # container verify done
    "jump": b"jumping to",                  # about to run the payload
    "app": b"spi_boot_app: running",        # payload is alive
}
WINDOW_MS = 1500.0                          # BOOT_ABORT_WINDOW_MS in the sample


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dev", default="/dev/ttyACM0")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--window", type=float, default=4.0,
                    help="seconds to read after each reset")
    ap.add_argument("--settle", type=float, default=0.3,
                    help="seconds to let the reader settle before resetting")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4444, help="openocd telnet port")
    ap.add_argument("--cmd", default="reset run")
    return ap.parse_args()


def one_run(args, run):
    ser = serial.Serial(args.dev, 115200, timeout=0.02)
    ser.reset_input_buffer()
    chunks = []
    done = threading.Event()

    def reader():
        while not done.is_set():
            try:
                data = ser.read(4096)
            except Exception:
                return
            if data:
                chunks.append((time.monotonic(), data))

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    time.sleep(args.settle)

    sock = socket.create_connection((args.host, args.port), timeout=5.0)
    sock.settimeout(0.5)
    time.sleep(0.1)
    t0 = time.monotonic()
    sock.sendall(args.cmd.encode() + b"\n")
    try:
        sock.recv(4096)
    except Exception:
        pass

    time.sleep(args.window)
    done.set()
    th.join(timeout=1.0)
    sock.close()
    ser.close()

    lines = []
    buf = b""
    for ts, data in chunks:
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            lines.append((ts, line.rstrip(b"\r")))
    if buf:
        lines.append((chunks[-1][0] if chunks else t0, buf))

    fresh = [ts for ts, _ in chunks if ts >= t0]
    first = (fresh[0] - t0) * 1000.0 if fresh else None
    print(f"=== run {run}: first byte after reset "
          f"{'n/a (nothing came back)' if first is None else f'{first:+.1f} ms'}")

    seen = {}
    for ts, line in lines:
        if ts >= t0:
            for name, pat in MARKS.items():
                if pat in line and name not in seen:
                    seen[name] = (ts - t0) * 1000.0
        txt = line.decode("utf-8", "replace")
        if txt.strip():
            print(f"  {(ts - t0) * 1000.0:+9.1f} ms | {txt}")

    if seen:
        marks = " ".join(f"{k}={v:+.1f}" for k, v in seen.items())
        # The window is a plain sleep, so subtracting it turns "verify" into
        # the work the loader actually did (policy + digest + signature).
        extra = ""
        if "verify" in seen and "window" in seen:
            extra = (f" post-window={seen['verify'] - seen['window'] - WINDOW_MS:+.1f}")
        print(f"  [marks] {marks}{extra}")
    return first


def main():
    args = parse_args()
    firsts = []
    for run in range(1, args.runs + 1):
        got = one_run(args, run)
        if got is not None:
            firsts.append(got)
    if firsts:
        firsts.sort()
        print(f"\nreset -> first byte: n={len(firsts)} min={firsts[0]:.1f} "
              f"median={firsts[len(firsts) // 2]:.1f} max={firsts[-1]:.1f} ms")


if __name__ == "__main__":
    main()
