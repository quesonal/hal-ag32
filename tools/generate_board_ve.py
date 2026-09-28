#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Render <board_dir>/board.ve from the `agm,agrv2k-pins` node in
# Zephyr's preprocessed zephyr.dts. Consumed by build_bitstream.sh
# before invoking the SDK gen_vlog tool, which only reads
# <board_dir>/board.ve.
#
# It also renders <board_dir>/board.generated.asf for the electrical
# attributes the same node asks for (agm,pull-ups / agm,pull-downs):
# AG32 implements pulls in the CPLD configuration, so they are ASF
# assignments, not pinctrl cells. build_bitstream.sh feeds that file to
# pre_logic.tcl, which folds it into <logic_dir>/board.asf.
#
# Why: the 4 AgRV2K board variants (agrv2k_103/303/407/test) used to
# each carry a hand-edited 68-line board.ve. The pinout is now
# expressed as a standard Zephyr devicetree node — dts/riscv/agm/
# agrv2k-pins.dtsi (shared by all 4 boards via the include chain in
# each board.dts). This script reads that node out of
# ${PROJECT_BINARY_DIR}/zephyr.dts (the cmake-preprocessed, fully
# expanded dts with includes resolved) and emits board.ve.
#
# See the board.ve notes for the full architecture.
#
# Usage:
#   generate_board_ve.py --dts <path> [--board-dir <dir>] [--out <path>]
#                        [--allow-pin-conflicts]
#
# Required:
#   --dts PATH          Zephyr-preprocessed dts (${PROJECT_BINARY_DIR}/zephyr.dts).
#
# Optional:
#   --board-dir DIR     Where to write board.ve (default: cwd).
#                       Used to also write a gitignored sibling file.
#   --out PATH          Override output path (default: <board-dir>/board.ve).
#   --allow-pin-conflicts
#                       Render despite duplicate pin bindings (exit 6
#                       becomes a warning). Env: AGM_ALLOW_PIN_CONFLICTS.
#   --quiet             Suppress the "wrote ..." status line.
#
# Exit codes:
#   0  success
#   1  parse / IO error
#   2  bad arguments
#   3  agm,agrv2k-pins node not found in dts
#   4  array length mismatch (e.g. mcu-functions vs mcu-pins)
#   5  missing required property (sysclk-frequency, hseclk-frequency)
#   6  duplicate pin binding (a pin claimed twice, or a name bound twice)

import argparse
import collections
import os
import re
import sys

# Match an entire node block (greedy on `{ ... }`, balanced). Used to
# pull out the agrv2k_pins { ... } body out of zephyr.dts.
NODE_BLOCK_RE = re.compile(
    r"agrv2k_pins\s*:\s*agrv2k-pins\s*\{"
    r"(?P<body>(?:[^{}]|\{[^{}]*\})*)\}",
    re.DOTALL,
)

# Property lines in zephyr.dts look like:
#   mcu-functions = "UART0_UARTRXD", "UART0_UARTTXD";
#   mcu-pins      = < 0x45 >, < 0x44 >;
#   sysclk-frequency = < 0xbebc200 >;
#
# Trailing `;` is required by dts syntax. Comments /* ... */ may
# appear after the value (`/* in foo.dtsi:42 */`).
PROP_STRING_ARRAY_RE = re.compile(
    r'^\s*(?P<name>\w[\w,\-]*)\s*=\s*(?P<values>(?:"(?:[^"\\]|\\.)*"\s*,?\s*)+)\s*;',
    re.MULTILINE,
)
PROP_INT_ARRAY_RE = re.compile(
    r'^\s*(?P<name>\w[\w,\-]*)\s*=\s*(?P<values>(?:<[^>]*>\s*,?\s*)+)\s*;',
    re.MULTILINE,
)
PROP_INT_SINGLE_RE = re.compile(
    r"^\s*(?P<name>\w[\w,\-]*)\s*=\s*<\s*(?P<value>0x[0-9a-fA-F]+|\d+)\s*>\s*;",
    re.MULTILINE,
)
STRING_LITERAL_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
INT_CELL_RE = re.compile(r"<\s*(0x[0-9a-fA-F]+|\d+)\s*>")

# Properties that are parallel arrays by contract. They must stay lists
# even when the dts holds a single cell (`mcu-pins = <15>;`), otherwise
# PROP_INT_SINGLE_RE turns them into an int and every zip()/len() down
# the line raises TypeError.
PARALLEL_ARRAY_PROPS = frozenset((
    "mcu-functions", "mcu-pins",
    "cpld-signals", "cpld-pins", "cpld-directions",
    "mcu-cpld-functions", "mcu-cpld-signals",
))

# Same single-cell trap, no parallelism: these are pin lists that happen
# to be often one entry long (`agm,pull-ups = <69>;`).
PAD_LIST_PROPS = frozenset((
    "agm,pull-ups", "agm,pull-downs",
    "agm,drive-strength-pins", "agm,drive-strength-ma",
    # Not an electrical attribute: the case-A rows that also carry `:INPUT`
    # (see render_ve). It is in here for the same reason -- it is a pin list
    # that is often one entry long, and a single-cell `<92>` parses as an int.
    "agm,mcu-input-pins",
))


def parse_string_array(prop_text):
    """Split `"foo", "bar"` into ['foo', 'bar']. Empty list if blank."""
    return [m.group(1) for m in STRING_LITERAL_RE.finditer(prop_text)]


def parse_int_array(prop_text):
    """Split `< 0x1 >, < 0x2 >` into [1, 2]. Empty list if blank."""
    return [int(m.group(1), 0) for m in INT_CELL_RE.finditer(prop_text)]


def extract_node_body(dts_text):
    """Return the body of agrv2k_pins { ... } (everything between the
    braces), or raise if not found.
    """
    m = NODE_BLOCK_RE.search(dts_text)
    if not m:
        raise LookupError(
            "agm,agrv2k-pins node not found in dts. Did the board "
            "include <agm/agrv2k-pins.dtsi>?")
    return m.group("body")


def parse_props(body):
    """Pull out every property in the node body into a dict. Arrays
    become lists; single cells stay as ints — except for the parallel
    pin arrays, which are normalised back to lists (see
    PARALLEL_ARRAY_PROPS).
    """
    props = {}
    for m in PROP_STRING_ARRAY_RE.finditer(body):
        props[m.group("name")] = parse_string_array(m.group("values"))
    for m in PROP_INT_ARRAY_RE.finditer(body):
        if m.group("name") in props:
            continue  # already captured as string array (shouldn't match here)
        props[m.group("name")] = parse_int_array(m.group("values"))
    for m in PROP_INT_SINGLE_RE.finditer(body):
        name = m.group("name")
        # Skip if a multi-cell array already claimed it.
        if name in props and len(props[name]) != 1:
            continue
        props[name] = int(m.group("value"), 0)
    for name in PARALLEL_ARRAY_PROPS | PAD_LIST_PROPS:
        if isinstance(props.get(name), int):
            props[name] = [props[name]]
    return props


def check_parallel(props, *array_names):
    """Raise if any of the named arrays have mismatched length."""
    lens = {n: len(props.get(n, [])) for n in array_names}
    distinct = set(lens.values())
    if len(distinct) > 1:
        raise ValueError(
            "parallel arrays have mismatched lengths: " +
            ", ".join(f"{n}={l}" for n, l in lens.items()))


def check_bindings(props):
    """Detect ambiguous pin bindings in the pin map.

    Every case A / case B row becomes one pin assignment in the .ve, so a
    pin claimed twice (or a name bound twice) is either a typo or a
    deliberate multi-load / fan-out. gen_vlog's own rules
    (platforms/AgRV/etc/gen_vlog, DeviceInfo.addPin) are direction-aware:

      - two rows on one pin are fine while every row is an input
        ("multiple loads driven by that device pin"), and rejected as soon
        as one row can drive the pin ("<pin> is used for both <a> and <b>");
      - one function on two pins is rejected for input functions and
        allowed for outputs (fan-out).

    This generator has no function-direction table (that lives in
    gen_vlog's FUNC_PINS), so it cannot reproduce those rules: it flags
    every duplicate and lets the caller confirm with
    --allow-pin-conflicts. Being loud here is the point — the alternative
    is a Quartus-time error, or a bitstream that routes something other
    than the pin map says.

    The one combination that is legal: an MCU function (case A) and a CPLD
    signal (case B) sharing a pin. The bitstream wires those two together
    in its internal cross-bar, and samples/slave_spi depends on it. Those
    land in `notes`, never in `errors`.

    Case C rows carry no pin, so only a repeated MCU function name is
    ambiguous there. A CPLD signal listed in both case B and case C is the
    documented "expose it on a pin *and* to user logic" pattern.

    Returns (errors, notes), both lists of human-readable strings.
    """
    errors = []
    notes = []

    def check_pairs(label, names, pins):
        """Cross-check one case's name/pin arrays; return {pin: [names]}."""
        by_name = {}
        by_pin = {}
        for name, pin in zip(names, pins):
            by_name.setdefault(name, []).append(pin)
            by_pin.setdefault(pin, []).append(name)
        for name, pin_list in sorted(by_name.items()):
            if len(pin_list) > 1:
                errors.append(
                    f"{label}: '{name}' is bound to more than one pin (" +
                    ", ".join(f"PIN_{p}" for p in pin_list) + ")")
        for pin, name_list in sorted(by_pin.items()):
            if len(name_list) > 1:
                errors.append(
                    f"{label}: PIN_{pin} is claimed by more than one entry (" +
                    ", ".join(f"'{n}'" for n in name_list) + ")")
        return by_pin

    a_by_pin = check_pairs("mcu-functions",
                           props.get("mcu-functions", []),
                           props.get("mcu-pins", []))
    b_by_pin = check_pairs("cpld-signals",
                           props.get("cpld-signals", []),
                           props.get("cpld-pins", []))

    for fn, count in sorted(collections.Counter(
            props.get("mcu-cpld-functions", [])).items()):
        if count > 1:
            errors.append(
                f"mcu-cpld-functions: '{fn}' is listed {count} times")

    for pin in sorted(set(a_by_pin) & set(b_by_pin)):
        notes.append(
            f"PIN_{pin} is shared by MCU function "
            f"{'/'.join(a_by_pin[pin])} and CPLD signal "
            f"{'/'.join(b_by_pin[pin])} (bitstream cross-bar)")

    # A case A row for a function that is *also* a case C function is not
    # what puts that function on the pin: the fabric routes the CPLD signal
    # instead, and the function name may never appear in the wrapper at all.
    # On the vendor example board SPI0_SI_IO0 is a
    # port of the SPI macro, reachable only through the bidir wire si_io0,
    # so its `SPI0_SI_IO0 PIN_92:INPUT` row produced no wrapper logic -- and
    # tools/check_pinctrl.py then cannot find the function in the netlist.
    c_signals = {}
    for fn, sig in zip(props.get("mcu-cpld-functions", []),
                       props.get("mcu-cpld-signals", [])):
        c_signals.setdefault(fn, []).append(sig)
    sig_pins = {}
    for pin, names in b_by_pin.items():
        for name in names:
            sig_pins.setdefault(name, pin)
    a_names = {name for names in a_by_pin.values() for name in names}
    for fn in sorted(set(c_signals) & a_names):
        via = [f"'{s}' (PIN_{sig_pins[s]})" if s in sig_pins else f"'{s}'"
               for s in c_signals[fn]]
        notes.append(
            f"'{fn}' has both a case A row and a case C row -> "
            f"{'/'.join(via)}: gen_vlog may route it through the CPLD signal "
            f"and leave the case A row with no wrapper logic of its own "
            f"(check the netlist before relying on that row)")

    return errors, notes


def fmt_pin(n):
    """Render pin number as PIN_<N>."""
    return f"PIN_{n}"


def labeled_node_props(dts_text, label):
    """Properties of the `label: node { ... }` with the given label, or
    None when the label is absent. Only the node's own body is parsed --
    brace counting, so nested nodes cannot confuse it."""
    m = re.search(rf"(?m)^\s*{re.escape(label)}\s*:\s*[\w\-@]+\s*\{{", dts_text)
    if not m:
        return None
    depth = 1
    i = m.end()
    while i < len(dts_text) and depth:
        c = dts_text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    body = dts_text[m.end():i - 1]
    return parse_props(body)


def check_clocks(dts_text, props):
    """The pin map's clocks must equal the board's own clocks.

    `sysclk-frequency` reaches the bitstream (board.ve's SYSCLK line, so
    the fabric PLL), while &clk0 is the clock the drivers divide for
    UART/timers and &cpu0 is what the kernel tick counts. They are three
    consumers of one number, and DT has no way to alias them -- so the
    numbers are spelled out in three nodes, and this is where they are
    checked. A mismatch is always a bug: the serial console garbles, the
    tick drifts (k_msleep(1000) returns early), or the fabric runs at the
    wrong rate.

    Returns a list of problem strings (empty when consistent or when the
    dts does not have both nodes -- e.g. a bare pins-only render).
    """
    problems = []
    want = props.get("sysclk-frequency")
    if not isinstance(want, int):
        return problems
    for label in ("clk0", "cpu0"):
        node = labeled_node_props(dts_text, label)
        if node is None:
            continue
        got = node.get("clock-frequency")
        if isinstance(got, int) and got != want:
            problems.append(
                f"&{label} runs at {got // 1_000_000} MHz but the pin map "
                f"declares sysclk-frequency = {want // 1_000_000} MHz; the "
                f"board must state one number (AGM_SYSCLK_HZ) and use it in "
                f"both")
    return problems


def render_asf(props):
    """Build the ASF fragment for the electrical attributes the pin map
    asks for, or "" when there are none. Returns (text, notes).

    AG32 does pulls in the CPLD configuration, not in a runtime register:
    the vendor's own pull-up / drive-strength note gives the ASF
    spellings, and this renders exactly those:

        set_instance_assignment -name WEAK_PULL_UP_RESISTOR ON -to PIN_96
        set_instance_assignment -name CFG_KEEP -to PIN_96 2'b01 -extension
        set_instance_assignment -name CURRENT_STRENGTH -to PIN_96 16MA

    (pull-up has a second, equivalent spelling -- CFG_KEEP ... 2'b10 -- and
    using both would put two 40k resistors in parallel; pull-down has only
    the CFG_KEEP 2'b01 form. Drive strength is 2 mA steps up to 32 mA.)

    which is why they are rendered here (from the pins node, where the pin
    is named) rather than into the pinctrl cells: a cell picks which
    peripheral drives a bank/bit, it cannot configure a pin resistor.
    """
    ups = props.get("agm,pull-ups", [])
    downs = props.get("agm,pull-downs", [])
    st_pins = props.get("agm,drive-strength-pins", [])
    st_ma = props.get("agm,drive-strength-ma", [])
    if not ups and not downs and not st_pins:
        return "", []

    known = set(props.get("mcu-pins", [])) | set(props.get("cpld-pins", []))
    both = sorted(set(ups) & set(downs))
    if both:
        raise ValueError(
            "agm,pull-ups and agm,pull-downs both name " +
            ", ".join(fmt_pin(p) for p in both))
    if len(st_pins) != len(st_ma):
        raise ValueError(
            f"agm,drive-strength-pins has {len(st_pins)} entr(ies) but "
            f"agm,drive-strength-ma has {len(st_ma)}")
    for pin, ma in zip(st_pins, st_ma):
        if ma < 2 or ma > 32 or ma % 2:
            raise ValueError(
                f"{fmt_pin(pin)}: drive strength {ma} mA is outside the "
                f"vendor rule (2 mA steps, 2..32 mA)")

    notes = [f"{fmt_pin(p)} has a pull but is not in mcu-pins/cpld-pins, "
             f"so nothing is routed there" for p in sorted(set(ups) | set(downs))
             if p not in known]
    notes += [f"{fmt_pin(p)} has a drive strength but is not in "
              f"mcu-pins/cpld-pins, so nothing is routed there"
              for p in sorted(set(st_pins)) if p not in known]

    lines = ["# Generated by tools/generate_board_ve.py from the "
             "agm,agrv2k-pins node.",
             "# Pulls and drive strength are CPLD configuration, so they are "
             "baked into the bitstream;",
             "# there is no runtime register for them."]
    # Vendor spellings, quoted from the AG32 application note: pull-up
    # `-name WEAK_PULL_UP_RESISTOR ... ON`; pull-down only exists as the
    # CFG_KEEP 2'b01 form; drive strength `-name CURRENT_STRENGTH ... <N>MA`.
    for pins, name, value, extra in ((ups, "WEAK_PULL_UP_RESISTOR", "ON", ""),
                                     (downs, "CFG_KEEP", "2'b01", " -extension")):
        for pin in sorted(pins):
            lines.append(
                f"set_instance_assignment -name {name} -to {fmt_pin(pin)} "
                f"{value}{extra}")
    for pin, ma in zip(st_pins, st_ma):
        lines.append(
            f"set_instance_assignment -name CURRENT_STRENGTH -to "
            f"{fmt_pin(pin)} {ma}MA")
    return "\n".join(lines) + "\n", notes


def render_ve(props):
    """Build the board.ve text from parsed dts properties."""
    sysclk = props.get("sysclk-frequency")
    hseclk = props.get("hseclk-frequency")
    if not isinstance(sysclk, int) or not isinstance(hseclk, int):
        raise LookupError(
            "agm,agrv2k-pins is missing sysclk-frequency and/or "
            "hseclk-frequency — both are required.")

    out = []
    out.append(f"SYSCLK {sysclk // 1_000_000}")
    out.append(f"HSECLK {hseclk // 1_000_000}")
    out.append("")

    # Case A — MCU function → pin.
    mcu_fns = props.get("mcu-functions", [])
    mcu_pins = props.get("mcu-pins", [])
    mcu_dirs = props.get("mcu-directions", [])
    check_parallel(props, "mcu-functions", "mcu-pins")
    if mcu_dirs:
        # Optional, index-aligned; an empty entry means "no direction on this
        # row" (the vendor VE spells a pin the MCU only reads as
        # `... PIN_92:INPUT`).
        check_parallel(props, "mcu-functions", "mcu-pins", "mcu-directions")
    # Pins whose case-A row must also carry a direction. The vendor VE spells
    # those `SPI0_SI_IO0 PIN_92:INPUT`: the function's own direction is the
    # default (SI is an output), and the explicit INPUT adds the pin→fabric
    # path back. It is keyed by pin rather than by index because the rows that
    # need it sit at the end of a 20+ entry list, and an index-aligned array
    # reaching them would be 20 empty strings that a mid-list insert silently
    # shifts.
    read_back = sorted(props.get("agm,mcu-input-pins", []))
    unknown_rb = [p for p in read_back if p not in mcu_pins]
    if unknown_rb:
        raise ValueError(
            "agm,mcu-input-pins names " +
            ", ".join(fmt_pin(p) for p in unknown_rb) +
            ", which has no case-A row in mcu-functions/mcu-pins — add the "
            "function's own row, or drop the pin from agm,mcu-input-pins")
    for i, (fn, pin) in enumerate(zip(mcu_fns, mcu_pins)):
        direction = mcu_dirs[i].upper() if i < len(mcu_dirs) and mcu_dirs[i] else ""
        if not direction and pin in read_back:
            direction = "INPUT"
        out.append(f"{fn} {fmt_pin(pin)}" + (f":{direction}" if direction else ""))

    if mcu_fns:
        out.append("")

    # Case B — CPLD signal → pin with direction.
    sigs = props.get("cpld-signals", [])
    pins = props.get("cpld-pins", [])
    dirs_ = props.get("cpld-directions", [])
    check_parallel(props, "cpld-signals", "cpld-pins")
    if dirs_:
        # Optional: omitting the array keeps the default direction on
        # every entry, and a present array must still line up.
        check_parallel(props, "cpld-signals", "cpld-pins", "cpld-directions")
    for i, (sig, pin) in enumerate(zip(sigs, pins)):
        direction = dirs_[i] if i < len(dirs_) else ""
        direction = direction if direction else "inout"
        out.append(f"{sig} {fmt_pin(pin)}:{direction.upper()}")

    if sigs:
        out.append("")

    # Case C — MCU function ↔ CPLD signal (no pin).
    mc_fns = props.get("mcu-cpld-functions", [])
    mc_sigs = props.get("mcu-cpld-signals", [])
    mc_dirs = props.get("mcu-cpld-directions", [])
    check_parallel(props, "mcu-cpld-functions", "mcu-cpld-signals")
    if mc_dirs:
        # Same rule as case A: optional, index-aligned, empty = no direction.
        # The vendor VE carries a direction on this row too
        # (`SPI0_SI_IO0 si_io0:OUTPUT`).
        check_parallel(props, "mcu-cpld-functions", "mcu-cpld-signals",
                       "mcu-cpld-directions")
    for i, (fn, sig) in enumerate(zip(mc_fns, mc_sigs)):
        direction = mc_dirs[i].upper() if i < len(mc_dirs) and mc_dirs[i] else ""
        out.append(f"{fn} {sig}" + (f":{direction}" if direction else ""))

    if mc_fns:
        out.append("")

    # Trailing newline if we emitted anything.
    text = "\n".join(out)
    if text and not text.endswith("\n"):
        text += "\n"
    return text


def main():
    p = argparse.ArgumentParser(
        description="Render board.ve from agm,agrv2k-pins node in zephyr.dts",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument("--dts", required=True,
                   help="path to Zephyr-preprocessed dts "
                        "(${PROJECT_BINARY_DIR}/zephyr.dts)")
    p.add_argument("--board-dir", default=None,
                   help="board directory (default: cwd); used as the "
                        "default output location")
    p.add_argument("--out", default=None,
                   help="output path (default: <board-dir>/board.ve)")
    p.add_argument("--out-dir", default=None,
                   help="directory for board.ve and board.generated.asf "
                        "(default: the board dir; a Zephyr build passes its "
                        "build-dir logic/ so nothing lands in the source tree)")
    p.add_argument("--allow-pin-conflicts", action="store_true",
                   default=bool(os.environ.get("AGM_ALLOW_PIN_CONFLICTS")),
                   help="downgrade an ambiguous pin map (exit 6) to a "
                        "warning and render anyway. Only for pin maps that "
                        "are knowingly hand-tuned; see check_bindings(). "
                        "Also settable via AGM_ALLOW_PIN_CONFLICTS.")
    p.add_argument("--allow-clock-mismatch", action="store_true",
                   default=bool(os.environ.get("AGM_ALLOW_CLOCK_MISMATCH")),
                   help="do not fail (exit 7) when sysclk-frequency differs "
                        "from &clk0/&cpu0::clock-frequency. Only for a board "
                        "that deliberately runs its peripherals off another "
                        "clock. Also settable via AGM_ALLOW_CLOCK_MISMATCH.")
    p.add_argument("--quiet", action="store_true",
                   help="suppress the 'wrote ...' status line")
    args = p.parse_args()

    if not os.path.exists(args.dts):
        print(f"error: --dts file not found: {args.dts}", file=sys.stderr)
        return 2

    board_dir = os.path.abspath(args.board_dir or os.getcwd())
    # Where board.ve / board.generated.asf land. Both used to be written into
    # the *source* tree (<board_dir>), which is also where parallel twister
    # jobs regenerated them at the same time -- the build-dir logic/ directory
    # is the default when this runs from a Zephyr build (`--out-dir`), and the
    # board dir stays the fallback for a standalone run.
    out_dir = os.path.abspath(args.out_dir) if args.out_dir else board_dir
    os.makedirs(out_dir, exist_ok=True)
    out_path = args.out or os.path.join(out_dir, "board.ve")
    asf_path = os.path.join(out_dir, "board.generated.asf")

    try:
        with open(args.dts) as f:
            dts_text = f.read()
        body = extract_node_body(dts_text)
        props = parse_props(body)
        conflicts, notes = check_bindings(props)
        clock_problems = ([] if args.allow_clock_mismatch
                          else check_clocks(dts_text, props))
        rendered = render_ve(props)
        asf, asf_notes = render_asf(props)
    except LookupError as e:
        print(f"error: {e}", file=sys.stderr)
        return 3
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 4
    except KeyError as e:
        print(f"error: missing required property: {e}", file=sys.stderr)
        return 5
    except (OSError, IOError) as e:
        print(f"error: I/O failure: {e}", file=sys.stderr)
        return 1

    for note in notes + asf_notes:
        print(f"generate_board_ve.py: note: {note}", file=sys.stderr)
    if conflicts:
        severity = "warning" if args.allow_pin_conflicts else "error"
        for msg in conflicts:
            print(f"generate_board_ve.py: {severity}: {msg}", file=sys.stderr)
        if not args.allow_pin_conflicts:
            print(
                "generate_board_ve.py: refusing to render a pin map with "
                "duplicate bindings — fix the arrays above, or re-run with "
                "--allow-pin-conflicts / AGM_ALLOW_PIN_CONFLICTS=1 if the "
                "duplication is intended (gen_vlog accepts two input-only "
                "rows on one pin, and an output function on several pins).",
                file=sys.stderr)
            return 6

    for problem in clock_problems:
        print(f"generate_board_ve.py: error: {problem}", file=sys.stderr)
    if clock_problems:
        print("generate_board_ve.py: fix the numbers (one define per board, "
              "used by the pin map and by &clk0/&cpu0), or re-run with "
              "--allow-clock-mismatch / AGM_ALLOW_CLOCK_MISMATCH=1 if the "
              "difference is intended.", file=sys.stderr)
        return 7

    # Idempotent writes: skip when content matches (preserves mtime so
    # ninja downstream doesn't rebuild for a no-op touch). The ASF
    # fragment is written next to board.ve and removed again when the pin
    # map stops asking for pulls, so a stale assignment can't linger.
    def write_if_changed(path, text):
        existing = ""
        if os.path.exists(path):
            with open(path) as f:
                existing = f.read()
        if text == existing:
            return False
        if text == "":
            os.unlink(path)
            return True
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
        return True

    ve_changed = write_if_changed(out_path, rendered)
    asf_changed = write_if_changed(asf_path, asf)
    if not args.quiet:
        if ve_changed:
            print(f"generate_board_ve.py: wrote {out_path}\n"
                  f"  source = {args.dts}", file=sys.stderr)
        else:
            print(f"generate_board_ve.py: {out_path} up-to-date",
                  file=sys.stderr)
        if asf_changed:
            print(f"generate_board_ve.py: {'wrote' if asf else 'removed'} "
                  f"{asf_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
