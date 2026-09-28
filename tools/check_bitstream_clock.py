#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# check_bitstream_clock.py — is the bitstream about to be written compiled for
# the clock this firmware was built for?
#
# Why this exists
# ---------------
# k_busy_wait() (and the kernel tick, and every peripheral divider) derive from
# CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC, which the board devicetree sets from
# AGM_SYSCLK_HZ. That is a *build-time constant* while the
# clock itself comes from the fabric. If the two disagree, every wait and every
# baud rate scales by declared/actual -- and nothing on the chip can notice:
# the target and the measurement are both in the same counter's ticks, so the
# device's own numbers stay self-consistent. With a 100 MHz
# bitstream and a 200 MHz firmware: 2x wall time, no in-chip symptom, console
# readable only at the halved baud rate).
#
# The numbers come from the fabric's own build inputs, either of which ships
# next to a bitstream:
#
#   * the `.ve` (the vehicle file `Supra gen_logic` consumes), which states
#     them outright: "SYSCLK 200", "BUSCLK 100", "HSECLK 8" (MHz);
#   * or, when there is no `.ve`, the generated Verilog's PLL parameters:
#     SYSCLK = CLKIN_FREQ x (CLKFB_HIGH + 1) / (CLKOUT0_HIGH + CLKOUT0_LOW + 2).
#     Cross-checked on the three dev board bitstreams (two of which also have a
#     `.ve` to agree with): 100 MHz and 200 MHz come out exactly.
#
# Usage
#   check_bitstream_clock.py <bitstream.bin|.ve> \
#       --config <build>/zephyr/.config [--dts <build>/zephyr/zephyr.dts]
#   check_bitstream_clock.py board.bin --sysclk-hz 200000000 --require-ve
#
# Exit: 0 the numbers agree (or there was nothing to compare), 1 a mismatch,
# 2 the check could not be made. `west flash` runs this before it writes a
# bitstream (scripts/west_commands/runners/agrv_openocd.py);
# AGM_SKIP_CLOCK_CHECK=1 turns it off for one invocation.

import argparse
import os
import re
import sys

VE_RE = re.compile(r"(?m)^\s*(SYSCLK|BUSCLK|HSECLK)\s+(\d+)\s*$")
CONFIG_RE = re.compile(r"(?m)^CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC=(\d+)\s*$")
# The merged zephyr.dts is dtc output: hex, and spaces inside the cells.
DTS_CELL = r"<\s*(0x[0-9a-fA-F]+|\d+)\s*>"
DTS_HSE_RE = re.compile(r"(?m)^\s*hseclk-frequency\s*=\s*" + DTS_CELL)
DTS_FLASH_RE = re.compile(r"(?m)^\s*flash-max-frequency\s*=\s*" + DTS_CELL)

PLL_RE = {
    "hse": re.compile(r"defparam\s+pll_inst\.CLKIN_FREQ\s*=\s*\"([0-9.]+)\""),
    "fb": re.compile(r"defparam\s+pll_inst\.CLKFB_HIGH\s*=\s*8'd(\d+)"),
    "out_high": re.compile(r"defparam\s+pll_inst\.CLKOUT0_HIGH\s*=\s*8'd(\d+)"),
    "out_low": re.compile(r"defparam\s+pll_inst\.CLKOUT0_LOW\s*=\s*8'd(\d+)"),
    "out_bypass": re.compile(r"defparam\s+pll_inst\.CLKOUT0_BYPASS\s*=\s*1'b(\d)"),
}


class Problem(Exception):
    """The check itself cannot be made (not a mismatch)."""


def read(path):
    with open(path, "r", errors="replace") as fh:
        return fh.read()


def parse_ve(text):
    """{'SYSCLK': Hz, ...} for whatever the .ve states."""
    out = {}

    for name, mhz in VE_RE.findall(text):
        out[name] = int(mhz) * 1000000
    if not out:
        raise Problem("no SYSCLK/BUSCLK/HSECLK line in the .ve")

    return out


def parse_verilog(text):
    """{'SYSCLK': Hz, 'HSECLK': Hz} computed from the generated PLL params."""
    got = {}

    for key, regex in PLL_RE.items():
        m = regex.search(text)
        if m is None:
            return {}           # not a generated board.v -- stay quiet
        got[key] = m.group(1)

    if got["out_bypass"] == "1":
        return {}               # the PLL is bypassed; CLKOUT0 is not SYSCLK

    vco_hz = float(got["hse"]) * (int(got["fb"]) + 1) * 1000000
    divider = int(got["out_high"]) + int(got["out_low"]) + 2
    return {"SYSCLK": int(vco_hz / divider),
            "HSECLK": int(float(got["hse"]) * 1000000)}


def find_source(bitstream, explicit):
    """(path, kind) for the file that states the fabric's clock."""
    if bitstream.endswith(".ve"):
        return bitstream, "ve"
    if explicit:
        return explicit, "ve" if explicit.endswith(".ve") else "verilog"

    stem = os.path.splitext(bitstream)[0]
    if os.path.isfile(stem + ".ve"):
        return stem + ".ve", "ve"
    if os.path.isfile(stem + ".v"):
        return stem + ".v", "verilog"

    directory = os.path.dirname(os.path.abspath(bitstream)) or "."
    for suffix, kind in ((".ve", "ve"), (".v", "verilog")):
        siblings = sorted(f for f in os.listdir(directory)
                          if f.endswith(suffix) and not f.endswith("_routed.v"))
        if len(siblings) == 1:
            return os.path.join(directory, siblings[0]), kind

    return None, None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bitstream", help="the .bin to be written, or a .ve itself")
    ap.add_argument("--ve", help="the .ve/.v that bitstream was compiled from")
    ap.add_argument("--config", help="a Zephyr .config (the firmware's constant)")
    ap.add_argument("--dts", help="a merged zephyr.dts (for HSE and BUSCLK)")
    ap.add_argument("--sysclk-hz", type=lambda v: int(v, 0),
                    help="expected SYSCLK in Hz, when no .config is at hand")
    ap.add_argument("--require-ve", action="store_true",
                    help="fail when nothing states the bitstream's clock")
    args = ap.parse_args(argv)

    if args.config and args.sysclk_hz:
        ap.error("--config and --sysclk-hz are mutually exclusive")

    try:
        path, kind = find_source(args.bitstream, args.ve)
        if path is None:
            if args.require_ve:
                raise Problem(f"no .ve/.v next to {args.bitstream} (and nothing "
                              "passed with --ve): the fabric clock cannot be "
                              "checked")
            print(f"check_bitstream_clock: WARNING: nothing states the clock of "
                  f"{args.bitstream} -- the fabric clock is unverified (pass "
                  f"--ve or --require-ve to make this fatal)")
            return 0

        text = read(path)
        ve = parse_ve(text) if kind == "ve" else parse_verilog(text)
        if not ve:
            if args.require_ve:
                raise Problem(f"{path} states no usable clock")
            print(f"check_bitstream_clock: WARNING: {path} states no usable "
                  f"clock -- the fabric clock is unverified")
            return 0
        if "SYSCLK" not in ve:
            raise Problem(f"{path} states no SYSCLK")

        expect_sysclk = args.sysclk_hz
        if args.config:
            m = CONFIG_RE.search(read(args.config))
            if m is None:
                raise Problem(f"{args.config} has no "
                              "CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC")
            expect_sysclk = int(m.group(1))

        comparisons = []
        if expect_sysclk is not None:
            comparisons.append(("SYSCLK", ve["SYSCLK"], expect_sysclk,
                                f"{os.path.basename(path)} ({kind}) vs "
                                + os.path.basename(args.config or "--sysclk-hz")))
        if args.dts:
            dts = read(args.dts)
            for key, regex, what in (("HSECLK", DTS_HSE_RE, "hseclk-frequency"),
                                     ("BUSCLK", DTS_FLASH_RE,
                                      "flash-max-frequency")):
                m = regex.search(dts)
                if key in ve and m is not None:
                    comparisons.append((key, ve[key], int(m.group(1), 0),
                                        f"{os.path.basename(path)} vs the dts "
                                        f"{what}"))

        if not comparisons:
            print("check_bitstream_clock: nothing to compare against "
                  "(pass --config, --sysclk-hz or --dts)")
            return 0

        bad = []
        for name, have, want, where in comparisons:
            mark = "ok" if have == want else "MISMATCH"
            print(f"  {name:7s} bitstream {have // 1000000:>4} MHz  "
                  f"firmware {want // 1000000:>4} MHz  {mark}  [{where}]")
            if have != want:
                bad.append((name, have, want))

        if bad:
            print("\ncheck_bitstream_clock: REFUSING -- the fabric would clock "
                  "this firmware at the wrong rate", file=sys.stderr)
            for name, have, want in bad:
                print(f"  {name}: bitstream {have} Hz, build expects {want} Hz",
                      file=sys.stderr)
            print("  k_busy_wait(), the tick and the UART dividers all follow "
                  "the build's constant, so every one of them would scale by "
                  f"{want // 1000000}/{have // 1000000}).",
                  file=sys.stderr)
            return 1

        print("check_bitstream_clock: OK")
        return 0
    except Problem as e:
        print(f"check_bitstream_clock: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
