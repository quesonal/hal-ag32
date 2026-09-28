#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Render a pinctrl-state fragment (agm,pins cells) from the pin list that
# a board dts / sample overlay declares, plus the SoC AF table.
#
# Why this is not derived from the bitstream: the (bank,bit) of an MCU
# function is fixed in silicon (tools/agm_af_pins.yaml, transcribed from
# the SDK's AltaRiscv.h). The fabric decides which *pin* a function lands
# on, and the fabric's pin->GPIO fan-out is many-to-many (PIN_57 feeds
# gpio7 bit 5 and bit 6), so the fabric can only answer "is this function
# routed, and on which pin". Generating from the fabric is ambiguous
# exactly where it matters.
#
# Input : the merged, cmake-preprocessed dts (${PROJECT_BINARY_DIR}/zephyr.dts)
#         -- the sample/board overlay has already replaced mcu-functions /
#         mcu-pins by then, and dtc has already folded the AGM_PINCTRL()
#         cells into literals. Feeding the un-folded neighbour file
#         (zephyr.dts.pre, or a plain .dtsi/overlay) is a hard error, not an
#         empty state -- see the guard in main().
# Output: a dts fragment that overrides the pinctrl states' agm,pins.
#
# Usage:
#   generate_pinctrl_dtsi.py --dts <zephyr.dts> --out <pinctrl-x.dtsi>
#                            [--af tools/agm_af_pins.yaml]
#                            [--source-note "..."] [--check]
#
#   --check  do not write; exit non-zero if the *effective* agm,pins in the
#            dts already differs from what would be generated (this is the
#            freshness gate for a committed fragment).
#
# Exit codes: 0 ok, 1 parse/IO error, 2 bad args, 3 agrv2k-pins node
# missing, 4 array length mismatch, 5 binding/AF table problem or an input
# that dtc has not folded yet (macros / arithmetic still in the cells),
# 6 freshness mismatch (--check), 7 --out points into a west workspace's
# modules/ tree.

import argparse
import os
import re
import sys

try:
    import yaml
except ImportError:  # pragma: no cover
    sys.stderr.write("PyYAML missing (repo venv has it)\n")
    raise SystemExit(1)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

NODE_BLOCK_RE = re.compile(
    r"agrv2k_pins\s*:\s*agrv2k-pins\s*\{"
    r"(?P<body>(?:[^{}]|\{[^{}]*\})*)\}",
    re.DOTALL,
)
PROP_STRING_ARRAY_RE = re.compile(
    r'^\s*(?P<name>[\w,\-]+)\s*=\s*(?P<values>(?:"(?:[^"\\]|\\.)*"\s*,?\s*)+)\s*;',
    re.MULTILINE,
)
PROP_INT_ARRAY_RE = re.compile(
    r"^\s*(?P<name>[\w,\-]+)\s*=\s*(?P<values>(?:<[^>]*>\s*,?\s*)+)\s*;",
    re.MULTILINE,
)
STRING_LITERAL_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
INT_CELL_RE = re.compile(r"<\s*(0x[0-9a-fA-F]+|\d+)\s*>")
# One cell is a literal only after dtc folded it:
#   merged dts    agm,pins = < 0x179 >, < 0x104 >;
#   zephyr.dts.pre    <((((7) & 0xf) << 0) | (((6) & 0xf) << 4) | ...)>
#   source dtsi       <AGM_PINCTRL(7, 6, AGM_PINCTRL_OUTPUT)>
# The last two carry no decodable bank/bit, so they are reported instead of
# being read as "this state has no cells".
FOLDED_CELL_RE = re.compile(r"\A\s*(?:0x[0-9a-fA-F]+|\d+)\s*\Z")

# Labeled nodes look like `eth0_default: eth0_default { ... };` in the merged
# zephyr.dts. Nested braces mean a regex cannot delimit the body (the
# `pinctrl: pinctrl {` container swallows every state inside it), so nodes
# are walked with a brace counter instead. The label and the node name are
# allowed to differ (`my_label: uart0_default { ... }`) -- dts permits it and
# dtc keeps both, so keying on the label must not require them to match.
LABELED_NODE_RE = re.compile(r"(?P<label>\w+)\s*:\s*(?P<name>[\w,\-@]+)\s*\{")
CHILD_NODE_RE = re.compile(r"\w+\s*:\s*\w+\s*\{")
# In the cmake-preprocessed dts the AGM_PINCTRL() macro is already expanded:
#   agm,pins = < 0x179 >, < 0x104 >, ... ;
# and AGM_PINCTRL(bank, pin, dir) == (bank & 0xf) | (pin & 0xf) << 4
#                                   | (dir & 0x3) << 8
# so cells are decoded rather than string-matched. (A firmware-only
# checkout never needs the macro text; the generator re-emits it.)
# The `(?:/\*.*?\*/)?` between cells is for hand-written states that carry a
# per-cell comment (`<0x100>, /* RX */ <0x101>`): without it the whole
# property fails to match and the state disappears from the comparison.
PINS_RE = re.compile(
    r"agm,pins\s*=\s*(?P<value>(?:<\s*[^>]*>\s*(?:/\*.*?\*/\s*)?,?\s*"
    r"(?:/\*.*?\*/\s*)?)+)\s*;",
    re.DOTALL)
# dtc renders a property whose array is empty as a valueless property, so
# the `agm,pins = <>;` placeholder in agrv2k.dtsi comes out of a real build
# as `agm,pins;` -- same meaning (no cells), different spelling.
PINS_EMPTY_RE = re.compile(r"agm,pins\s*;")
CELL_TEXT_RE = re.compile(r"<\s*([^>]*?)\s*>")


def parse_props(body):
    props = {}
    for m in PROP_STRING_ARRAY_RE.finditer(body):
        props[m.group("name")] = STRING_LITERAL_RE.findall(m.group("values"))
    for m in PROP_INT_ARRAY_RE.finditer(body):
        props[m.group("name")] = [int(v, 0) for v in INT_CELL_RE.findall(m.group("values"))]
    return props


def walk_labeled_nodes(dts_text):
    """Yield (label, body) for every `x: x { ... }` node, bodies via a
    brace counter. Container nodes are yielded too; callers filter."""
    for m in LABELED_NODE_RE.finditer(dts_text):
        start = m.end()
        depth = 1
        i = start
        while i < len(dts_text) and depth:
            c = dts_text[i]
            # Comments and strings can hold braces of their own; skipping
            # them keeps the counter honest (`/* } */` used to truncate the
            # body, which silently dropped the state).
            if c == "/" and dts_text.startswith("/*", i):
                end = dts_text.find("*/", i + 2)
                i = len(dts_text) if end < 0 else end + 2
                continue
            if c == "/" and dts_text.startswith("//", i):
                end = dts_text.find("\n", i)
                i = len(dts_text) if end < 0 else end + 1
                continue
            if c == '"':
                i += 1
                while i < len(dts_text) and dts_text[i] != '"':
                    i += 2 if dts_text[i] == "\\" else 1
                i += 1
                continue
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
            i += 1
        yield m.group("label"), dts_text[start:i - 1]


def pins_shape(dts_text):
    """What every labeled node says about agm,pins.

    Returns a dict with three disjoint views of the same walk, because
    "no cells" has three very different causes and the callers report them
    differently:

      * ``cells``    {label: [(bank, bit, dir_int), ...]} -- decodable cells
      * ``empty``    {label, ...} -- agm,pins present but holding no cell,
                     i.e. the ``agm,pins = <>;`` placeholder that a missing
                     ``<agm/pinctrl-*.dtsi>`` include leaves behind
      * ``unfolded`` {label: [cell text, ...]} -- dtc has not folded the
                     cells yet (zephyr.dts.pre, or a plain .dtsi/overlay),
                     so no bank/bit can be decoded from them

    A pinctrl state has no child nodes -- skip containers such as
    `pinctrl: pinctrl {` so their first nested agm,pins is not mistaken
    for the container's own."""
    shape = {"cells": {}, "empty": set(), "unfolded": {}}
    for label, body in walk_labeled_nodes(dts_text):
        if CHILD_NODE_RE.search(body):
            continue
        parsed = parse_agm_pinctrl(body)
        if parsed is None:
            continue
        cells, unfolded = parsed
        if unfolded:
            shape["unfolded"][label] = unfolded
        elif cells:
            shape["cells"][label] = cells
        else:
            shape["empty"].add(label)
    return shape


def effective_cells(dts_text):
    """{state: [(bank, bit, dir_int), ...]} for the states that carry cells."""
    return pins_shape(dts_text)["cells"]


def declared_states(dts_text):
    """Labels that declare agm,pins, in whatever form the cells are in.

    "Does this pinctrl state exist?" is a different question from "what do
    its cells say" -- agrv2k.dtsi declares the generated states with an empty
    placeholder and the hand-written ones with un-folded macros, and both are
    real states. See check_af_table.py."""
    shape = pins_shape(dts_text)
    return set(shape["cells"]) | shape["empty"] | set(shape["unfolded"])


def decode_cell(value):
    """AGM_PINCTRL(bank, pin, dir) -> (bank, pin, dir_int)."""
    return (value & 0xf, (value >> 4) & 0xf, (value >> 8) & 0x3)


def parse_agm_pinctrl(text):
    """(cells, unfolded) for the first agm,pins in `text`, or None if there
    is no agm,pins at all. `unfolded` lists the cell texts that are not
    literals, which means the dts never went through cpp+dtc."""
    m = PINS_RE.search(text)
    if not m:
        # `agm,pins;` -- the empty array dtc writes for `agm,pins = <>;`
        return ([], []) if PINS_EMPTY_RE.search(text) else None
    cells, unfolded = [], []
    for raw in CELL_TEXT_RE.findall(m.group("value")):
        if not raw:
            continue
        if FOLDED_CELL_RE.match(raw):
            cells.append(decode_cell(int(raw, 0)))
        else:
            unfolded.append(raw)
    return cells, unfolded


def folded_input_problem(dts_text):
    """A message when the input still carries macro/arithmetic cells, else
    None. `--check` and `--out` both compare against cells they decoded from
    the dts, so an un-folded input must fail loudly instead of looking like
    "every state is empty"."""
    unfolded = pins_shape(dts_text)["unfolded"]
    if not unfolded:
        return None
    states = sorted(unfolded)[:3]
    sample = unfolded[states[0]][0]
    return (f"{len(unfolded)} pinctrl state(s) still carry un-folded agm,pins "
            f"cells ({summarize(states)}; e.g. <{sample}>). This looks like a "
            f"non-preprocessed dts: pass the merged "
            f"${{PROJECT_BINARY_DIR}}/zephyr.dts, not zephyr.dts.pre or a "
            f"source .dtsi/overlay (run `west build` first).")


SDK_DIRS = ("in", "out", "inout")
DIRS = ("input", "output", "no-dir", "bidir")

# Mirrors AGM_GPIO_BANK_COUNT / AGM_GPIO_PINS_PER_BANK in
# soc/agm/agrv2k/agm_sys.h. pinctrl_configure_pins() rejects a cell outside
# these ranges with -EINVAL, which shows up as a failed peripheral init long
# after the AF table was edited; checking here points at the table instead.
BANK_COUNT = 10
PINS_PER_BANK = 8


class _DuplicateKey(ValueError):
    """A duplicated mapping key in the AF table."""


class _UniqueKeyLoader(yaml.SafeLoader):
    """PyYAML silently keeps the last of duplicated keys. The table is 114
    transcribed rows, so a duplicated function name has to be an error --
    otherwise the dropped line is only caught by the vendor-header
    comparison, and that one is skipped on a firmware-only checkout."""

    def construct_mapping(self, node, deep=False):
        seen = set()
        for key_node, _ in node.value:
            key = self.construct_object(key_node, deep=True)
            if isinstance(key, str):
                if key in seen:
                    raise _DuplicateKey(key)
                seen.add(key)
        return super().construct_mapping(node, deep=deep)


def load_af(path):
    """The SoC AF table. Every function the SoC can hand a pin to is
    listed with its AFSEL bank/bit (required) and the header's direction
    marker (required); `dir` and `state` are the routing decisions and
    are only present for functions a pinctrl state owns today."""
    with open(path, encoding="utf-8") as f:
        try:
            doc = yaml.load(f, Loader=_UniqueKeyLoader)
        except _DuplicateKey as e:
            raise ValueError(f"{path}: duplicate key {e.args[0]!r} -- PyYAML "
                             f"would silently keep the last one") from None
        except yaml.YAMLError as e:
            raise ValueError(f"{path}: {e}") from None
    table = doc.get("af_pins")
    if not isinstance(table, dict):
        raise ValueError(f"{path}: missing 'af_pins:' mapping")
    for fn, spec in table.items():
        for key in ("bank", "bit", "sdk"):
            if key not in spec:
                raise ValueError(f"{path}: {fn} is missing '{key}'")
        if not isinstance(spec["bank"], int) or not 0 <= spec["bank"] < BANK_COUNT:
            raise ValueError(f"{path}: {fn} has bank {spec['bank']!r} -- the "
                             f"SoC has banks 0..{BANK_COUNT - 1}")
        if not isinstance(spec["bit"], int) or not 0 <= spec["bit"] < PINS_PER_BANK:
            raise ValueError(f"{path}: {fn} has bit {spec['bit']!r} -- a bank "
                             f"has bits 0..{PINS_PER_BANK - 1}")
        if spec["sdk"] not in SDK_DIRS:
            raise ValueError(f"{path}: {fn} has bad sdk marker {spec['sdk']!r}")
        if "dir" in spec and spec["dir"] not in DIRS:
            raise ValueError(f"{path}: {fn} has bad dir {spec['dir']!r}")
        if "state" in spec and "dir" not in spec:
            raise ValueError(f"{path}: {fn} names a state but no dir -- the "
                             f"cell's DIR is a decision, not a default")
    return table


DIR_INT = {"input": 0, "output": 1, "no-dir": 2}
DIR_MACRO = {0: "AGM_PINCTRL_INPUT", 1: "AGM_PINCTRL_OUTPUT", 2: "AGM_PINCTRL_NO_DIR"}


def cells_for(fn, spec):
    """Decoded cells, in the same form the dts carries after preprocessing."""
    bank, bit = spec["bank"], spec["bit"]
    if spec["dir"] == "bidir":
        return [(bank, bit, DIR_INT["output"]), (bank, bit, DIR_INT["input"])]
    return [(bank, bit, DIR_INT[spec["dir"]])]


def build(functions, af, warn):
    """{state: [(bank,bit,dir)]} for a pin list.

    A function the AF table does not know at all is a warning: either
    the name is new (add it) or it is a fabric pin signal that has no
    AFSEL bit (GPIOb_n, USB0_ID) and therefore no cell by construction.
    A function the table knows but that no state owns is only a note --
    the shared board pin list declares UART1 pins the fabric routes and
    no driver uses."""
    states = {}
    unknown, stateless = [], []
    for fn in functions:
        spec = af.get(fn)
        if spec is None:
            unknown.append(fn)
            continue
        if "state" not in spec:
            stateless.append(fn)
            continue
        states.setdefault(spec["state"], []).extend(cells_for(fn, spec))
    if unknown:
        warn(f"{len(unknown)} pin-list name(s) have no AF entry and no cell "
             f"({summarize(unknown)}) -- fabric pin signals such as GPIOb_n "
             f"and USB0_ID have no AFSEL bit by construction")
    if stateless:
        warn(f"{len(stateless)} declared function(s) are known to the AF "
             f"table but no pinctrl state owns them, so nothing is emitted "
             f"for them ({summarize(stateless)})")
    return states


def summarize(names, keep=4):
    names = sorted(names)
    tail = f", ... ({len(names)} total)" if len(names) > keep else ""
    return ", ".join(names[:keep]) + tail


def compare_state(state, got, want, af):
    """Problems with one state's effective cells.

    `want` is what the pin list implies; `got` is what the merged dts
    carries. Two rules:

      * every wanted cell must be present (a missing cell means the
        fragment is stale or the include is gone);
      * every present cell must belong to a *complete* AF function of
        that state -- extras that no function in the table produces are
        typos or ghosts (this is what catches a stray RX_CLK (7,6): the
        table deliberately has no MAC0_RX_CLK entry because the bitstream
        does not route it).

    Cells contributed by another included fragment (e.g. the board-level
    UART0 fragment inside a MAC-only sample) are therefore accepted, and
    a pin-list omission is caught by the netlist comparison instead --
    see check_pinctrl.py's "routed but not declared" pass.

    A third rule covers the one place where the *order* of the cells
    matters: a row that carries more than one cell (`bidir` in the AF
    table, e.g. MDIO's OUT+IN pair) leaves the pin in the state the *last*
    cell wrote, because pinctrl_configure_pins() walks the cells in order.
    Same-row cells are therefore compared as a sequence, not as a set.
    """
    problems = []
    missing = [c for c in want if c not in got]
    if missing:
        problems.append(f"{state}: missing cells {missing} "
                        f"(the pin list declares them)")
    explainable = set()
    for fn, spec in af.items():
        if spec.get("state") != state:
            continue
        cells = cells_for(fn, spec)
        if all(c in got for c in cells):
            explainable |= set(cells)
    extra = [c for c in got if c not in explainable]
    if extra:
        problems.append(f"{state}: cells {extra} belong to no AF function "
                        f"of this state (ghost / wrong bank-bit)")
    want_rows, got_rows = row_dirs(want), row_dirs(got)
    for row, dirs in sorted(want_rows.items()):
        got_dirs = got_rows.get(row, [])
        if len(dirs) > 1 and sorted(got_dirs) == sorted(dirs) \
                and got_dirs != dirs:
            problems.append(
                f"{state}: gpio{row[0]} bit{row[1]} lists its {len(dirs)} "
                f"cells as {got_dirs}, expected {dirs} -- the last cell of a "
                f"row decides the DIR the pin is left with")
    return problems


def row_dirs(cells):
    """[(bank, bit, dir), ...] -> {(bank, bit): [dir, ...]} in cell order."""
    rows = {}
    for bank, bit, d in cells:
        rows.setdefault((bank, bit), []).append(d)
    return rows


def render(states, header_lines):
    lines = ["/* SPDX-License-Identifier: Apache-2.0 */",
             "/*",
             " * AUTO-GENERATED by tools/generate_pinctrl_dtsi.py -- DO NOT EDIT.",
             " * Re-run it after changing the pin list or the AF table.",
             " *"]
    lines += [" * " + l for l in header_lines]
    lines += [" */", "", "#include <zephyr/dt-bindings/pinctrl/agm-agrv2k-pinctrl.h>", ""]
    for state in sorted(states):
        cells = states[state]
        lines.append(f"&{state} {{")
        lines.append("\tagm,pins =")
        for i, (bank, bit, d) in enumerate(cells):
            sep = ";" if i == len(cells) - 1 else ","
            lines.append(f"\t\t<AGM_PINCTRL({bank}, {bit}, {DIR_MACRO[d]})>{sep}")
        lines.append("};")
        lines.append("")
    return "\n".join(lines)


def mirror_guard(out, repo, note=None):
    """Refuse the one footgun that actually happened: generating into
    <workspace>/modules/hal_ag32 instead of the tree this script lives in.

    Everything under modules/hal_ag32 that a workspace build sees is a
    mirror (tools/devsync.sh push/restore); an edit there is thrown away
    by `restore --apply` and silently makes the build and the repo
    disagree in the meantime. Only the case where --out is *outside* this
    repo can be that mistake -- a checkout that really does live at
    modules/hal_ag32 is fine, but it is worth a note, because the same path
    is also what a devsync mirror looks like from the inside and there is no
    marker on disk that tells the two apart."""
    out_abs = os.path.abspath(out)
    if out_abs == repo or out_abs.startswith(repo + os.sep):
        if note is not None and f"{os.sep}modules{os.sep}hal_ag32" in repo:
            note(f"this tree is {repo} -- if another tree mirrors into it "
                 f"(rsync/git) an edit here can be overwritten by the next "
                 f"sync; edit the tree you develop in and sync that.")
        return None
    parts = out_abs.split(os.sep)
    for i, part in enumerate(parts):
        if part == "modules" and i + 1 < len(parts) and parts[i + 1].startswith("hal_ag32"):
            workspace = os.sep.join(parts[:i]) or os.sep
            return (f"--out is under {os.path.join(workspace, 'modules', 'hal_ag32')}, "
                    f"the mirror of this module that a west workspace build reads "
                    f"-- an edit there can be overwritten by the next sync, and the "
                    f"generated fragment belongs in the tree this script lives in "
                    f"({repo}). Retry with --force if you really mean that path.")
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dts", required=True)
    ap.add_argument("--out")
    ap.add_argument("--af", default=os.path.join(HERE, "agm_af_pins.yaml"))
    ap.add_argument("--source-note", action="append", default=[])
    ap.add_argument("--check", action="store_true",
                    help="exit 6 if the dts' effective agm,pins differs")
    ap.add_argument("--force", action="store_true",
                    help="allow --out inside a west workspace's modules/ tree")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    def warn(msg):
        if not args.quiet:
            sys.stderr.write(f"note: {msg}\n")

    if not args.check and not args.out:
        sys.stderr.write("--out is required unless --check is used\n")
        return 2

    if args.out and not args.force:
        problem = mirror_guard(args.out, REPO, warn)
        if problem:
            sys.stderr.write(f"{problem}\n")
            return 7

    try:
        text = open(args.dts, encoding="utf-8").read()
    except OSError as e:
        sys.stderr.write(f"cannot read {args.dts}: {e}\n")
        return 1

    problem = folded_input_problem(text)
    if problem:
        sys.stderr.write(f"{args.dts}: {problem}\n")
        return 5

    m = NODE_BLOCK_RE.search(text)
    if not m:
        sys.stderr.write(f"{args.dts}: agm,agrv2k-pins node not found\n")
        return 3
    props = parse_props(m.group("body"))
    functions = props.get("mcu-functions", [])
    pins = props.get("mcu-pins", [])
    if not functions:
        sys.stderr.write(f"{args.dts}: mcu-functions is empty or missing\n")
        return 3
    if len(functions) != len(pins):
        sys.stderr.write(f"mcu-functions ({len(functions)}) and mcu-pins "
                         f"({len(pins)}) length mismatch\n")
        return 4

    try:
        af = load_af(args.af)
    except (OSError, ValueError) as e:
        sys.stderr.write(f"{e}\n")
        return 5

    states = build(functions, af, warn)

    if args.check:
        shape = pins_shape(text)
        have = shape["cells"]
        bad = 0
        for state, want in sorted(states.items()):
            if state in shape["empty"]:
                sys.stderr.write(f"{state}: present in {args.dts} but holds no "
                                 f"cells (`agm,pins = <>;`, expected "
                                 f"{len(want)}) -- the generated fragment that "
                                 f"fills this state is not included\n")
                bad += 1
                continue
            got = have.get(state, [])
            if not got:
                sys.stderr.write(f"{state}: not a pinctrl state in {args.dts} "
                                 f"(expected {len(want)} cells)\n")
                bad += 1
                continue
            for problem in compare_state(state, got, want, af):
                sys.stderr.write(f"{problem}\n")
                bad += 1
        if bad:
            sys.stderr.write(
                f"hint: regenerate the fragment from this same dts:\n"
                f"hint:   tools/generate_pinctrl_dtsi.py --dts {args.dts} "
                f"--out <fragment>.dtsi\n"
                f"hint: and write it to the fragment this board/sample "
                f"includes -- grep -rn 'pinctrl-.*\\.dtsi' in "
                f"dts/riscv/agm/agrv2k-board.dtsi and "
                f"samples/<sample>/boards/ -- the board-level dtsi is the "
                f"one agm,agrv2k-board.dtsi includes\n")
            return 6
        if not args.quiet:
            print(f"{args.dts}: pinctrl states match the pin list "
                  f"({', '.join(sorted(states))})")
        return 0

    header = [f"Source dts : {os.path.relpath(args.dts)}",
              f"AF table   : {os.path.relpath(args.af)}"]
    header += args.source_note
    out = render(states, header)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(out)
    if not args.quiet:
        print(f"wrote {args.out} ({len(states)} state(s), "
              f"{sum(len(v) for v in states.values())} cells)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
