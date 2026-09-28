#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# kill_on_pattern.py — run a command in its own process group and SIGKILL that
# group the moment a pattern shows up on its output.
#
# Why it exists: tools/agm_rdp_tear_test.sh wants to interrupt the option write
# exactly between the erase and the program. The driver prints
# "Option bytes are erased" after the erase and only then issues the program, so
# the target is a few milliseconds wide -- a shell watcher polling the log every
# 20 ms either matched the previous write's line or arrived after the program
# (several samples, none torn). This reads the pipe line by line
# (openocd flushes per line) and signals at once; --delay shifts the signal
# inside that gap, which is the knob for sweeping it.
#
# Usage:
#   kill_on_pattern.py --pattern "Option bytes are erased" \
#       --log /tmp/session.log --shell 'openocd -f … -c "…"' [--delay S]
#
# Exit code is the command's, or 137 when the group was killed.

import argparse
import os
import signal
import subprocess
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pattern", required=True)
    ap.add_argument("--shell", required=True,
                    help="the command to run (through sh -c)")
    ap.add_argument("--log", required=True, help="where its output is kept")
    ap.add_argument("--delay", type=float, default=0.0,
                    help="seconds to wait after the pattern before killing")
    ap.add_argument("--timeout", type=float, default=180.0,
                    help="give up on the session after this long")
    args = ap.parse_args()

    t0 = time.time()
    killed = False

    with open(args.log, "w") as log:
        proc = subprocess.Popen(["sh", "-c", args.shell],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, bufsize=1, start_new_session=True)
        for line in proc.stdout:
            log.write(line)
            log.flush()
            if not killed and args.pattern in line:
                if args.delay:
                    time.sleep(args.delay)
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                killed = True
                print(f"killed the session {time.time() - t0:.3f} s in, "
                      f"{args.delay * 1000:.1f} ms after {line.strip()!r}",
                      file=sys.stderr)
            if time.time() - t0 > args.timeout:
                break
        proc.wait()

    print("session ended" + (" (killed)" if killed else " (ran to its end)"),
          file=sys.stderr)
    return proc.returncode if not killed else 137


if __name__ == "__main__":
    sys.exit(main())
