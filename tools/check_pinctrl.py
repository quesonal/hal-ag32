#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Check the three things that hand-written pinctrl tuples get wrong
#
#
#   1. function -> pin : what the board dts / sample overlay claims
#                        (mcu-functions/mcu-pins) must be what the
#                        bitstream's netlist actually wires.
#   2. pin -> bank/bit : the pin must really feed the GPIO bit the SoC AF
#                        table assigns to that function. The fabric's
#                        fan-out is many-to-many (PIN_57 feeds gpio7 bit 5
#                        *and* bit 6), so this is a membership test, not
#                        an equality test.
#   3. state content   : the effective agm,pins of each pinctrl state must
#                        be exactly the cells the AF table implies -- no
#                        missing cells, no extras. This is what would have
#                        caught MDC at (4,5) instead of (9,7), PIN_57
#                        labelled as RX_CLK, and the ghost RX_CLK cell.
#   4. completeness    : for the states this pin list *touches*, it must
#                        declare every function the bitstream routes.
#                        That is what catches the overlays that were
#                        missing MAC0_CRS while eth0_default carried its
#                        cell. Functions of states the list does not
#                        touch are only notes -- a sample overlay
#                        deliberately describes a subset of its bitstream.
#
# Usage:
#   check_pinctrl.py --dts <zephyr.dts> [--netlist <example_board.v>]
#                    [--ve <board.ve>] [--af tools/agm_af_pins.yaml] [--quiet]
#
# --netlist is optional so a firmware-only checkout can still run check 3.
# It must be the netlist of the bitstream the pin list describes; passing
# one from another bitstream reports real mismatches (SPI0 pins differ
# between the spi_full and the spi_full_mac bitstreams, for instance).
#
# --ve (default: the netlist's sibling <stem>.ve, when it exists) lets the
# netlist checks see a route that the netlist cannot express: Case C joins an
# MCU function to a CPLD signal *inside* the fabric, so the function name
# never appears in the wrapper at all (`SPI0_SI_IO0 si_io0` in board.ve, and
# the netlist only says `assign PIN_92_in = si_io0;`). Without it those
# functions are reported as "does not route this function at all".
#
# When no board.ve is there, the same alias is read from the pins node the
# dts declares (mcu-cpld-functions / mcu-cpld-signals). That half of the
# chain is then the pin list's own claim rather than the bitstream's input
# spec, which the notes say out loud; --no-ve turns both off and leaves only
# what the netlist itself proves.
#
# Exit codes: 0 ok, 1 parse/IO error, 2 bad args, 5 AF table problem,
# 7 a check failed.

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from generate_pinctrl_dtsi import (  # noqa: E402
    NODE_BLOCK_RE, build, compare_state, declared_states, load_af, parse_props,
    folded_input_problem, pins_shape, row_dirs, summarize,
)

# For the notes: the AGM_PINCTRL_* encodings, spelled out.
DIR_NAME = {0: "input", 1: "output", 2: "no-dir"}

# --- netlist parsing ---------------------------------------------------
# Function -> pin, from the wrapper's pin assignments:
#   assign PIN_57_in = MAC0_TX_CLK;                          (input)
#   assign MAC0_MDC = PIN_58_out_en ? PIN_58_out_data : 1'bz;(output)
NET_FUNC_TO_PAD_IN = re.compile(r"^\s*assign\s+PIN_(\d+)_in\s*=\s*(\w+)\s*;",
                                re.MULTILINE)
NET_FUNC_TO_PAD_OUT = re.compile(
    r"^\s*assign\s+(\w+)\s*=\s*PIN_(\d+)_out_en\s*\?", re.MULTILINE)

# Pin -> GPIO fan-out. Outputs are explicit per pin:
#   assign PIN_58_out_data = gpio9_io_out_data[7];
# Inputs are a positional 8-entry concatenation, MSB first:
#   wire [7:0] gpio7_io_in = {PIN_47_in, PIN_57_in, PIN_57_in, ...};
NET_PAD_OUT_BIT = re.compile(
    r"^\s*assign\s+PIN_(\d+)_out_data\s*=\s*gpio(\d+)_io_out_data\[(\d+)\]\s*;",
    re.MULTILINE)
# A pin that is wired through a CPLD signal (Case B/C) spells the same
# assignment with the signal's name instead of PIN_n:
#   assign sck_out_data = gpio4_io_out_data[5];   (sck PIN_93:OUTPUT)
NET_SIG_OUT_BIT = re.compile(
    r"^\s*assign\s+(\w+)_out_data\s*=\s*gpio(\d+)_io_out_data\[(\d+)\]\s*;",
    re.MULTILINE)
NET_GPIO_IN_CONCAT = re.compile(
    r"wire\s*\[\s*7\s*:\s*0\s*\]\s*gpio(\d+)_io_in\s*=\s*\{(?P<items>[^}]*)\}",
    re.MULTILINE)

# --- board.ve parsing --------------------------------------------------
# The three association kinds the pin map (and gen_vlog) use:
#   Case A  <FUNCTION> PIN_<n>                  e.g. MAC0_MDC     PIN_58
#   Case B  <signal>   PIN_<n>:<DIRECTION>      e.g. si_io0       PIN_92:OUTPUT
#   Case C  <FUNCTION> <signal>                 e.g. SPI0_SI_IO0  si_io0
# Case C is the one the netlist cannot show: the function is wired to the
# signal *inside* the fabric, so only the signal reaches the pins.
VE_CASE_A_RE = re.compile(
    r"^\s*(?P<fn>[A-Za-z_]\w*)\s+PIN_(?P<pin>\d+)\s*"
    r"(?::\s*(?P<dir>\w+))?\s*(?:#.*)?$", re.MULTILINE)
VE_CASE_C_RE = re.compile(
    r"^\s*(?P<fn>[A-Z][A-Z0-9_]*)\s+(?P<sig>[A-Za-z_]\w*)\s*"
    r"(?::\s*(?P<dir>\w+))?\s*(?:#.*)?$", re.MULTILINE)


def parse_ve(text):
    """(function -> CPLD signal, signal -> pin) from a board.ve file.

    A `PIN_<n>` on the right-hand side is Case A (an MCU function) or Case B
    (a CPLD signal); anything else on the right of an upper-case name is
    Case C. Which of Case A/B a line is does not matter here: both give
    "this name lands on this pin"."""
    case_c, name_to_pin = {}, {}
    for m in VE_CASE_A_RE.finditer(text):
        name_to_pin.setdefault(m.group("fn"), int(m.group("pin")))
    for m in VE_CASE_C_RE.finditer(text):
        sig = m.group("sig")
        if sig.startswith("PIN_"):
            continue        # a Case A / Case B line, already handled above
        case_c.setdefault(m.group("fn"), sig)
    return case_c, name_to_pin


def route_views(case_c, name_to_pin, source):
    """What resolve_route() needs, plus where the alias came from.

    `source` ends up in the note, because the two halves of a route are not
    equally verified: board.ve is the bitstream's own input (gen_vlog ate it
    to produce the netlist), while the dts's mcu-cpld-* rows are the pin
    list's claim about the fabric. Either way the signal -> pin half still
    has to be found in the netlist (or in the pin list's own pins)."""
    return {"case_c": case_c, "name_to_pin": name_to_pin, "source": source}


def route_views_from_ve(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        case_c, name_to_pin = parse_ve(f.read())
    return route_views(case_c, name_to_pin, f"board.ve ({path})")


def route_views_from_dts(props):
    """The Case C alias alone, read from the pins node itself.

    Used when no board.ve is available: `mcu-cpld-functions` /
    `mcu-cpld-signals` name the same pairing the VE would, and that pairing
    is the one thing a netlist can never express. The pin half is
    deliberately left empty -- taking it from the same pin list would make
    every "does this function land on this pin" question answer itself, and
    the netlist is the witness that matters."""
    case_c = dict(zip(props.get("mcu-cpld-functions", []),
                      props.get("mcu-cpld-signals", [])))
    return route_views(case_c, {}, "the pin list (mcu-cpld-functions)")


def resolve_route(fn, func_to_pin, ve):
    """(pin, note) for `fn` when the netlist does not name the function
    directly. Two ways it can still be routed:

      * a Case C row (`SPI0_SI_IO0 si_io0`) puts it on a CPLD signal, and
        that signal reaches the pin -- the netlist only ever shows the
        signal, never the function;
      * the alias source simply puts the function on the pin itself (a
        Case A row) and gen_vlog did not turn that row into a net the
        parser can see (it does that for functions the design exposes as
        its own ports).

    pin is None when neither applies."""
    direct = func_to_pin.get(fn)
    if direct is not None or ve is None:
        return direct, None
    sig = ve["case_c"].get(fn)
    if sig is not None:
        routed = func_to_pin.get(sig)
        where = "the netlist"
        if routed is None:
            routed = ve["name_to_pin"].get(sig)
            where = ve["source"]
        if routed is not None:
            return routed, (f"{fn}: routed through the CPLD cross-bar as "
                            f"'{sig}' ({ve['source']} pairs them, {where} puts "
                            f"it on PIN_{routed}); the netlist never names "
                            f"{fn} itself")
    routed = ve["name_to_pin"].get(fn)
    if routed is not None:
        return routed, (f"{fn}: {ve['source']} puts it on PIN_{routed} but the "
                        f"netlist never names {fn} -- that half of the route "
                        f"is unverified")
    return None, None


def parse_netlist(text, warn=None):
    """(function -> pin, pin -> {(bank, bit)}) from the wrapper.

    `warn` is called for a pin that the same gpio*_io_in concatenation lists
    twice: that is a wrapper-level declaration (one pin can be tapped by
    several GPIO bits, e.g. PIN_57 feeding bits 5 and 6 of bank 7) rather
    than evidence that the pin physically drives both, so it is worth
    saying out loud but never a failure."""
    func_to_pin = {}
    for pin, fn in NET_FUNC_TO_PAD_IN.findall(text):
        func_to_pin.setdefault(fn, int(pin))
    for fn, pin in NET_FUNC_TO_PAD_OUT.findall(text):
        func_to_pin.setdefault(fn, int(pin))

    pin_to_bits = {}
    for pin, bank, bit in NET_PAD_OUT_BIT.findall(text):
        pin_to_bits.setdefault(int(pin), set()).add((int(bank), int(bit)))
    for m in NET_GPIO_IN_CONCAT.finditer(text):
        bank = int(m.group(1))
        items = [i.strip() for i in m.group("items").split(",")]
        seen = {}
        # MSB first: [7], [6], ... [0]
        for idx, item in enumerate(items):
            bit = 7 - idx
            mm = re.match(r"PIN_(\d+)_in$", item)
            if mm:
                pin = int(mm.group(1))
                pin_to_bits.setdefault(pin, set()).add((bank, bit))
                seen.setdefault(pin, []).append(bit)
        if warn is not None:
            for pin, bits in sorted(seen.items()):
                if len(bits) > 1:
                    warn(f"PIN_{pin} appears {len(bits)} times in the gpio"
                         f"{bank}_io_in concatenation (bits "
                         f"{'/'.join(str(b) for b in sorted(bits))}): the "
                         f"wrapper declares that tap, which is not proof "
                         f"that the pin physically feeds both")
    # The same fan-out, spelled with a CPLD signal instead of a pin name:
    # the signal reaches the pin (Case B) and the GPIO bit drives the
    # signal, so the pin does feed that bit. Without this the pins SPI0
    # reaches through csn/sck/si_io0 have no fan-out entry at all and check
    # 2 fails on them even though the route is real.
    for name, bank, bit in NET_SIG_OUT_BIT.findall(text):
        if re.match(r"PIN_\d+$", name):
            continue  # a pin, already collected above
        pin = func_to_pin.get(name)
        if pin is not None:
            pin_to_bits.setdefault(pin, set()).add((int(bank), int(bit)))
    return func_to_pin, pin_to_bits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dts", required=True)
    ap.add_argument("--netlist")
    ap.add_argument("--ve", help="board.ve the netlist was generated from "
                                 "(default: the netlist's sibling <stem>.ve, "
                                 "when that file exists; without it the "
                                 "Case C alias is read from the pin list)")
    ap.add_argument("--no-ve", action="store_true",
                    help="do not look for a sibling board.ve and do not take "
                         "a Case C alias from the pin list either: only what "
                         "the netlist itself proves counts")
    ap.add_argument("--af", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                 "agm_af_pins.yaml"))
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    notes = []
    ve_path = args.ve
    if ve_path is None and not args.no_ve and args.netlist:
        sibling = os.path.splitext(args.netlist)[0] + ".ve"
        if os.path.isfile(sibling):
            ve_path = sibling
    try:
        text = open(args.dts, encoding="utf-8").read()
    except OSError as e:
        sys.stderr.write(f"cannot read {args.dts}: {e}\n")
        return 1
    shape = pins_shape(text)
    if shape["unfolded"]:
        sys.stderr.write(f"{args.dts}: {folded_input_problem(text)}\n")
        return 5

    m = NODE_BLOCK_RE.search(text)
    if not m:
        sys.stderr.write(f"{args.dts}: agm,agrv2k-pins node not found\n")
        return 1
    props = parse_props(m.group("body"))
    functions = props.get("mcu-functions", [])
    pins = props.get("mcu-pins", [])
    if len(functions) != len(pins):
        sys.stderr.write(f"mcu-functions ({len(functions)}) vs mcu-pins "
                         f"({len(pins)}) length mismatch\n")
        return 7

    try:
        af = load_af(args.af)
    except (OSError, ValueError) as e:
        sys.stderr.write(f"{e}\n")
        return 5

    failures = []

    # --- 3. state content -------------------------------------------------
    states = build(functions, af, notes.append)
    have = shape["cells"]
    for state, want in sorted(states.items()):
        if state in shape["empty"]:
            # `agm,pins = <>;` is what a missing <agm/pinctrl-*.dtsi>
            # include leaves behind: a valid, empty array, so nothing else
            # fails -- soc.c gates no bank and pinctrl_configure_pins()
            # sees pin_cnt == 0, and the peripheral init succeeds anyway.
            failures.append(
                f"{state}: present in the merged dts but holds no cells "
                f"(`agm,pins = <>;`, expected {len(want)}) -- the generated "
                f"fragment that fills this state is not included")
            continue
        failures.extend(compare_state(state, have.get(state, []), want, af))

    # A state the pin list does not cover is fine -- its cells come from a
    # fragment included by another layer (the board default list) or are
    # still hand-written. Reported once, as a note.
    uncovered = [s for s in sorted(declared_states(text))
                 if s not in states]
    if uncovered:
        notes.append(f"{len(uncovered)} state(s) in the merged dts are not "
                     f"covered by this pin list, their cells come from an "
                     f"included fragment or were hand-written "
                     f"({summarize(uncovered)})")

    # A merged state can also carry *more* cells than this pin list
    # implies: another layer's fragment already filled it in. Not a
    # failure (the extra cells belong to real AF functions), but worth
    # saying out loud -- samples/slave_spi is exactly this case today.
    for state, want in sorted(states.items()):
        extra = sorted(c for c in have.get(state, []) if c not in want)
        if extra:
            notes.append(f"{state}: {len(extra)} cell(s) beyond what this pin "
                         f"list implies ({extra}) -- another included fragment "
                         f"or a hand-written state supplies them")
        # A row with more than one cell is a bidir pair (the AF table's
        # `bidir`, today only MDIO). Say what the two cells mean and that
        # their order is part of the contract -- compare_state() fails the
        # state if they come back the other way round.
        for (bank, bit), dirs in sorted(row_dirs(want).items()):
            if len(dirs) > 1:
                notes.append(
                    f"{state}: gpio{bank} bit{bit} carries {len(dirs)} cells "
                    f"({'/'.join(DIR_NAME[d] for d in dirs)}) -- a bidir row: "
                    f"the two "
                    f"directions must differ, and their order decides the DIR "
                    f"the pin is left with")

    # --- 1 & 2. against the netlist --------------------------------------
    if args.netlist:
        try:
            net = open(args.netlist, encoding="utf-8", errors="replace").read()
        except OSError as e:
            sys.stderr.write(f"cannot read {args.netlist}: {e}\n")
            return 1
        ve = None
        if ve_path:
            try:
                ve = route_views_from_ve(ve_path)
            except OSError as e:
                sys.stderr.write(f"cannot read {ve_path}: {e}\n")
                return 1
            notes.append(f"{ve['source']}: {len(ve['case_c'])} Case C "
                         f"function(s), {len(ve['name_to_pin'])} name->pin "
                         f"row(s)")
        elif not args.no_ve:
            ve = route_views_from_dts(props)
            if ve["case_c"]:
                notes.append(
                    f"no board.ve next to {args.netlist}; resolving the "
                    f"{len(ve['case_c'])} Case C alias(es) from "
                    f"{ve['source']} -- that pairing is the pin list's own "
                    f"claim, not the fabric's (pass --ve <board.ve> for the "
                    f"bitstream's own spec, or --no-ve to ignore both)")
        func_to_pin, pin_to_bits = parse_netlist(net, notes.append)
        if not func_to_pin or not pin_to_bits:
            sys.stderr.write(
                f"{args.netlist}: looks like the wrong file (no PIN_*_in / "
                f"gpio*_io_in wiring found). The pre-route wrapper from "
                f"`prepare logic` (logic/board.vx, or tools/tests/fixtures/"
                f"example_board.v) is the input here; a post-route "
                f"*_routed.v has none of these nets.\n")
            return 1

        declared = set(functions)
        covered_states = {af[fn]["state"] for fn in declared
                          if "state" in af.get(fn, {})}
        undeclared = []

        # The pin list has to be complete for the states it touches: if
        # it declares (say) eth0_default's pins, it must declare *all* of
        # them, or a regenerated board.ve silently drops the one that is
        # missing (the LAN8720 overlays were missing MAC0_CRS/PIN_56 for a
        # week while eth0_default still carried its cell).
        for fn, spec in sorted(af.items()):
            if fn in declared or fn not in func_to_pin:
                continue
            where = (f"state {spec['state']}" if "state" in spec
                     else "no pinctrl state yet")
            msg = (f"{fn}: routed on PIN_{func_to_pin[fn]} by the bitstream "
                   f"but absent from mcu-functions ({where})")
            if spec.get("state") in covered_states:
                failures.append(msg)
            else:
                undeclared.append(f"{fn} PIN_{func_to_pin[fn]}")
        if undeclared:
            notes.append(f"{len(undeclared)} function(s) this bitstream "
                         f"routes belong to states this pin list does not "
                         f"touch, so they are not its business "
                         f"({summarize(undeclared)})")

        for fn, pin in zip(functions, pins):
            spec = af.get(fn)
            if spec is None:
                continue
            net_pin, via = resolve_route(fn, func_to_pin, ve)
            if net_pin is None:
                hint = ("" if ve is not None else
                        " -- if the bitstream routes it through the CPLD "
                        "cross-bar (a Case C row in board.ve), pass --ve "
                        "<board.ve>")
                failures.append(f"{fn}: dts says PIN_{pin}, but the netlist "
                                f"does not route this function at all{hint}")
                continue
            if via:
                notes.append(via)
            if net_pin != pin:
                failures.append(f"{fn}: dts says PIN_{pin}, netlist says "
                                f"PIN_{net_pin}")
                continue
            want = (spec["bank"], spec["bit"])
            if want not in pin_to_bits.get(pin, set()):
                if not pin_to_bits.get(pin):
                    failures.append(
                        f"{fn}: PIN_{pin} is not wired to any GPIO bit by this "
                        f"netlist, so the AFSEL route for gpio{want[0]} "
                        f"bit{want[1]} has no pin behind it -- either this "
                        f"bitstream does not bring PIN_{pin} out, or the "
                        f"netlist is from another bitstream")
                else:
                    failures.append(
                        f"{fn}: PIN_{pin} does not feed gpio{want[0]} "
                        f"bit{want[1]} in the netlist (it feeds "
                        f"{sorted(pin_to_bits.get(pin, set()))})")

    if not args.quiet:
        for note in notes:
            sys.stderr.write(f"note: {note}\n")
    if failures:
        for f in failures:
            sys.stderr.write(f"FAIL: {f}\n")
        return 7
    if not args.quiet:
        scope = "dts + netlist" if args.netlist else "dts"
        print(f"pinctrl check OK ({scope}): {len(functions)} pin bindings, "
              f"{sum(len(v) for v in states.values())} cells")
    return 0


if __name__ == "__main__":
    sys.exit(main())
