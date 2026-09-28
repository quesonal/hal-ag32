#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Check the SoC AF table (tools/agm_af_pins.yaml) against its two
# sources of truth:
#
#   * the vendor header -- when the SDK is installed -- must list exactly
#     the same functions with the same bank/bit/direction marker. This is
#     the completeness gate for the 114 transcribed rows: a typo, an
#     omission or a stale row all fail.
#   * the dts -- every `state:` the table names must exist as a pinctrl
#     state, and each `dir:` must be plausible for the header's direction
#     marker (an INPUT pin cannot be an `output` cell, ...).
#
# Runs without the SDK (firmware-only checkout): the header check is then
# skipped with a note, the dts checks still run.
#
# Usage:
#   check_af_table.py [--af tools/agm_af_pins.yaml] [--dts <dts>]
#                     [--sdk-header <AltaRiscv.h>] [--quiet]
#
# Exit codes: 0 ok, 1 parse/IO error, 2 bad args, 5 table problem,
# 8 AF table and header disagree.

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from generate_pinctrl_dtsi import (  # noqa: E402
    declared_states, load_af, summarize,
)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# One AF function in the vendor header is three consecutive defines:
#   #define MAC0_TXD0_AF_GPIO 9
#   #define MAC0_TXD0_AF_GPIO_MASK (0x1 << 1)
#   #define MAC0_TXD0_AF_GPIO_OUTPUT
HDR_COUNTERS = (
    re.compile(r"^#define\s+\w+_AF_GPIO\s+\d+\s*$", re.MULTILINE),
    re.compile(r"^#define\s+\w+_AF_GPIO_MASK\s+\(0x1\s*<<\s*\d+\)\s*$", re.MULTILINE),
    re.compile(r"^#define\s+\w+_AF_GPIO_(?:INPUT|OUTPUT|INOUT)\s*$", re.MULTILINE),
)
HDR_RE = re.compile(
    r"#define\s+(?P<fn>\w+)_AF_GPIO\s+(?P<bank>\d+)\s*\n"
    r"#define\s+(?P=fn)_AF_GPIO_MASK\s+\(0x1\s*<<\s*(?P<bit>\d+)\)\s*\n"
    r"#define\s+(?P=fn)_AF_GPIO_(?P<dir>INPUT|OUTPUT|INOUT)\b"
)
HDR_MARKER = {"INPUT": "in", "OUTPUT": "out", "INOUT": "inout"}

# Which cell directions are plausible for an AFSEL bit the header calls
# INPUT / OUTPUT / INOUT. This is deliberately one-sided: DIR is a board
# decision (the SDK's GPIO_AF_ENABLE writes AFSEL only), so `no-dir` is
# allowed everywhere; what must not happen is driving a pin the SoC can
# only read, or reading one it can only write.
DIR_FOR_MARKER = {
    "in": {"input", "no-dir"},
    "out": {"output", "no-dir"},
    "inout": {"no-dir", "bidir", "output", "input"},
}


def parse_header(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    table = {}
    for m in HDR_RE.finditer(text):
        table[m.group("fn")] = (int(m.group("bank")), int(m.group("bit")),
                                HDR_MARKER[m.group("dir")])
    for counter in HDR_COUNTERS:
        declared = len(counter.findall(text))
        if declared != len(table):
            raise ValueError(
                f"{path}: {declared} '{counter.pattern}' defines but "
                f"{len(table)} triples parsed -- the header's formatting "
                f"changed, fix the regex instead of trusting a partial table")
    return table


def dts_states(path):
    """State *names* declared by the dts, cells or not. --dts defaults to the
    source agrv2k.dtsi, where the generated states carry the `agm,pins = <>;`
    placeholder and the hand-written ones carry un-folded AGM_PINCTRL()
    macros; both are real states, and this check only asks whether the name
    exists."""
    return declared_states(open(path, encoding="utf-8").read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--af", default=os.path.join(HERE, "agm_af_pins.yaml"))
    ap.add_argument("--dts", default=os.path.join(REPO, "dts/riscv/agm/agrv2k.dtsi"))
    ap.add_argument("--sdk-header",
                    default=os.path.join(os.environ.get("AGRV_SDK_PATH",
                                                        os.path.expanduser("~/AgRV_pio")),
                                         "packages/framework-agrv_sdk/src/AltaRiscv.h"))
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    try:
        af = load_af(args.af)
    except (OSError, ValueError) as e:
        sys.stderr.write(f"{e}\n")
        return 5

    failures, header_failures, notes = [], [], []

    # --- dir vs the header's direction marker -----------------------------
    for fn, spec in sorted(af.items()):
        if "dir" not in spec:
            continue
        allowed = DIR_FOR_MARKER[spec["sdk"]]
        if spec["dir"] not in allowed:
            failures.append(f"{fn}: dir '{spec['dir']}' on a pin the header "
                            f"calls {spec['sdk'].upper()} (allowed: "
                            f"{sorted(allowed)})")

    # --- two *drivers* of one state on the same AFSEL bit ------------------
    #
    # One row (bank/bit) can carry a driving function and a sampling one at
    # the same time -- the vendor's AGRV2K logic note says it outright: "同一
    # 行的输入和输出外设引脚可以同时使用,例如 UART1_UARTRXD 可以和
    # GPTIMER2_CHN2 同时使用,但使用了二者任意一个均不可使用 GPIO6_3". The
    # pin's input path and output path are separate nets, so what is illegal
    # is two functions that would both drive that row.
    drivers = {}
    for fn, spec in sorted(af.items()):
        if "state" not in spec or spec["dir"] not in ("output", "bidir"):
            continue
        key = (spec["state"], spec["bank"], spec["bit"])
        if key in drivers:
            failures.append(
                f"{spec['state']}: {fn} and {drivers[key]} would both drive "
                f"gpio{spec['bank']} bit{spec['bit']}")
        drivers[key] = fn

    # --- the states the table names must exist ----------------------------
    if args.dts:
        try:
            states = dts_states(args.dts)
        except OSError as e:
            sys.stderr.write(f"cannot read {args.dts}: {e}\n")
            return 1
        named = {spec["state"] for spec in af.values() if "state" in spec}
        for state in sorted(named - states):
            failures.append(f"{state}: named by the AF table but not a "
                            f"pinctrl state in {os.path.relpath(args.dts)}")
        unused = sorted(states - named)
        if unused:
            notes.append(f"{len(unused)} pinctrl state(s) have no AF-table "
                         f"row (their pins are hand-written or already "
                         f"empty): {summarize(unused)}")

    # --- the vendor header is the completeness gate -----------------------
    header = None
    if args.sdk_header:
        try:
            header = parse_header(args.sdk_header)
        except OSError:
            notes.append(f"vendor header not found at {args.sdk_header}, "
                         f"skipping the completeness check (set "
                         f"AGRV_SDK_PATH or pass --sdk-header)")
        except ValueError as e:
            failures.append(str(e))

    if header is not None:
        for fn in sorted(set(header) - set(af)):
            header_failures.append(f"{fn} ({header[fn][0]}.{header[fn][1]}, "
                                   f"{header[fn][2]}): in "
                                   f"{os.path.basename(args.sdk_header)} but "
                                   f"missing from the AF table")
        for fn in sorted(set(af) - set(header)):
            header_failures.append(f"{fn}: in the AF table but not in "
                                   f"{os.path.basename(args.sdk_header)}")
        for fn in sorted(set(af) & set(header)):
            want, got = header[fn], (af[fn]["bank"], af[fn]["bit"], af[fn]["sdk"])
            if want != got:
                header_failures.append(
                    f"{fn}: table says bank {got[0]} bit {got[1]} {got[2]}, "
                    f"header says bank {want[0]} bit {want[1]} {want[2]}")

    if not args.quiet:
        for note in notes:
            sys.stderr.write(f"note: {note}\n")
        for f in failures + header_failures:
            sys.stderr.write(f"FAIL: {f}\n")
        if not failures and not header_failures:
            withstate = sum(1 for s in af.values() if "state" in s)
            ctx = (f", matches {os.path.basename(args.sdk_header)}"
                   if header is not None else ", header not checked")
            print(f"AF table OK: {len(af)} function(s), {withstate} with a "
                  f"pinctrl state ({len(af) - withstate} mapping-only){ctx}")

    if header_failures:
        return 8
    return 5 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
