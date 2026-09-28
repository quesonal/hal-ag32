#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Regression tests for the pin-routing tools (tools/generate_pinctrl_dtsi.py,
# tools/check_pinctrl.py, tools/check_af_table.py). They are pure Python and
# need no board, no SDK and no build tree:
#
#   python3 -m unittest discover -s tools/tests
#   tools/check_pin_routing.sh          # runs these plus the AF table check
#
# Each test pins down a failure mode that was found by review.

import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

FIXTURES = os.path.join(HERE, "fixtures")

from check_pinctrl import (  # noqa: E402
    parse_netlist, parse_ve, resolve_route, route_views,
)
import generate_board_ve as gen  # noqa: E402
from generate_board_ve import check_bindings  # noqa: E402
from generate_pinctrl_dtsi import (  # noqa: E402
    STRING_LITERAL_RE, build, compare_state, declared_states, effective_cells,
    load_af, pins_shape,
)

AF = os.path.join(TOOLS, "agm_af_pins.yaml")
DEFAULT_DTSI = os.path.join(REPO, "dts/riscv/agm/agrv2k.dtsi")

# Committed fragment -> the pin list(s) it is generated from. Every
# committed fragment must appear here; a pin list listed twice (the three
# lan8720 overlays) has to agree with the fragment on its own.
FRAGMENT_DIR = "dts/riscv/agm"
FRAGMENT_SOURCES = {
    "pinctrl-default.dtsi": ["dts/riscv/agm/agrv2k-pins.dtsi"],
    "pinctrl-devmac.dtsi": [
        "samples/lan8720_link/boards/agrv2k_407.overlay",
        "samples/lan8720_iperf/boards/agrv2k_407.overlay",
        "samples/lan8720_udp_direct/boards/agrv2k_407.overlay",
    ],
    "pinctrl-slavespi.dtsi": ["samples/slave_spi/boards/agrv2k_407.overlay"],
}

_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
_MCU_FUNCTIONS_RE = re.compile(
    r"mcu-functions\s*=(?P<values>(?:\s*\"[^\"]*\"\s*,?)+)\s*;")
_CELL_MACRO_RE = re.compile(
    r"AGM_PINCTRL\((\d+),\s*(\d+),\s*(AGM_PINCTRL_\w+)\)")
_DIR_INT = {"AGM_PINCTRL_INPUT": 0, "AGM_PINCTRL_OUTPUT": 1,
            "AGM_PINCTRL_NO_DIR": 2}

# A pin list that exercises the same pins as fixtures/example_board.v.
PINS_NODE = """
/ {
\tagrv2k_pins: agrv2k-pins {
\t\tcompatible = "agm,agrv2k-pins";
\t\tmcu-functions = %s;
\t\tmcu-pins = %s;
\t};
};
"""

SPI_FNS = ["SPI0_CSN", "SPI0_SCK", "SPI0_SI_IO0", "SPI0_SO_IO1"]
SPI_PADS = [96, 93, 92, 97]

# The four SPI0 state cells the AF table implies, in the folded form the
# merged dts carries: AGM_PINCTRL(bank, bit, dir) == bank | bit << 4 | dir << 8,
# so AGM_PINCTRL(4, 6, OUTPUT) == 0x164 and AGM_PINCTRL(0, 1, NO_DIR) == 0x210.
SPI_CELLS = "< 0x164 >, < 0x154 >, < 0x100 >, < 0x210 >"

# The pins are wired through CPLD signals, exactly as gen_vlog does it:
# SPI0_SI_IO0 never appears, si_io0 does.
SPI_NETLIST = """
module example_board (csn, sck, si_io0, so_io1);
output csn;
output sck;
inout  si_io0;
inout  so_io1;
assign csn = PIN_96_out_en ? PIN_96_out_data : 1'bz;
assign sck = PIN_93_out_en ? PIN_93_out_data : 1'bz;
assign PIN_92_in = si_io0;
assign PIN_97_in = so_io1;
assign csn_out_data = gpio4_io_out_data[6];
assign sck_out_data = gpio4_io_out_data[5];
assign si_io0_out_data = gpio0_io_out_data[0];
wire [7:0] gpio0_io_in = {1'b0, 1'b0, 1'b0, 1'b0, 1'b0, 1'b0, PIN_97_in, PIN_92_in};
wire [7:0] gpio4_io_in = {1'b0, 1'b0, 1'b0, 1'b0, 1'b0, 1'b0, 1'b0, 1'b0};
endmodule
"""

# The same netlist, but with no GPIO fan-out for the SPI pins at all -- what
# fixtures/example_board.v looks like for PIN_93/PIN_96, whose board.ve rows
# the design ports shadow. The unrelated MAC pin keeps pin_to_bits non-empty,
# so this is not mistaken for "the wrong file".
SPI_NETLIST_UNWIRED = """
module example_board (csn, sck);
output csn;
output sck;
assign csn = PIN_96_out_en ? PIN_96_out_data : 1'bz;
assign sck = PIN_93_out_en ? PIN_93_out_data : 1'bz;
assign MAC0_MDC = PIN_58_out_en ? PIN_58_out_data : 1'bz;
assign PIN_58_out_data = gpio9_io_out_data[7];
wire [7:0] gpio9_io_in = {1'b0, 1'b0, 1'b0, 1'b0, 1'b0, 1'b0, 1'b0, 1'b0};
endmodule
"""

SPI_VE = """
SYSCLK 200
SPI0_CSN    csn
SPI0_SCK    sck
SPI0_SI_IO0 si_io0:OUTPUT
csn         PIN_96:OUTPUT
sck         PIN_93:OUTPUT
si_io0      PIN_92:OUTPUT
so_io1      PIN_97:INPUT
SPI0_SO_IO1  PIN_97
"""


def run_tool(script, *args):
    return subprocess.run(
        [sys.executable, os.path.join(TOOLS, script), *args],
        capture_output=True, text=True)


def dts_with_pin_list(functions, pins, states=""):
    quoted = ", ".join(f'"{f}"' for f in functions)
    ints = ", ".join(f"< {p} >" for p in pins)
    return PINS_NODE % (quoted, ints) + states


def read_repo(rel):
    with open(os.path.join(REPO, rel), encoding="utf-8") as f:
        return f.read()


def pin_list_functions(rel):
    """mcu-functions of a pin list (the shared dtsi, or a sample overlay)."""
    text = _COMMENT_RE.sub(" ", read_repo(rel))
    m = _MCU_FUNCTIONS_RE.search(text)
    if not m:
        raise AssertionError(f"{rel}: no mcu-functions array")
    return STRING_LITERAL_RE.findall(m.group("values"))


def fragment_cells(rel):
    """{state: [(bank, bit, dir)]} from a committed fragment (macro form)."""
    states = {}
    for m in re.finditer(r"&(\w+)\s*\{\s*agm,pins\s*=(?P<v>.*?);\s*\};",
                         read_repo(rel), re.DOTALL):
        states[m.group(1)] = [(int(b), int(p), _DIR_INT[d]) for b, p, d
                              in _CELL_MACRO_RE.findall(m.group("v"))]
    return states


class TestPinParsing(unittest.TestCase):
    """dts parsing: the cell text must be folded, and the walker must not
    lose a state over a comment, a string or a label/name mismatch."""

    def test_folded_cells_decode(self):
        text = f"uart0_default: uart0_default {{\n agm,pins = {SPI_CELLS};\n}};"
        self.assertEqual(effective_cells(text)["uart0_default"],
                         [(4, 6, 1), (4, 5, 1), (0, 0, 1), (0, 1, 2)])

    def test_empty_placeholder_vs_absent_vs_unfolded(self):
        shape = pins_shape(
            "uart0_default: uart0_default {\n agm,pins = <>;\n};\n"
            "can0_default: can0_default {\n agm,pins = <AGM_PINCTRL(8, 7, "
            "AGM_PINCTRL_OUTPUT)>;\n};\n")
        self.assertEqual(shape["empty"], {"uart0_default"})
        self.assertEqual(list(shape["unfolded"]), ["can0_default"])
        # both are declared states -- check_af_table asks this question
        self.assertEqual(declared_states(
            "uart0_default: uart0_default {\n agm,pins = <>;\n};\n"),
            {"uart0_default"})

    def test_dtc_spells_an_empty_array_as_a_valueless_property(self):
        # A real `west build` renders `agm,pins = <>;` as `agm,pins;`
        # (a real `west build` renders it that way), which is the form a
        # missing <agm/pinctrl-*.dtsi> include leaves behind.
        shape = pins_shape(
            "uart0_default: uart0_default {\n"
            "\tagm,pins;          /* in .../empty_pins.overlay:2 */\n"
            "\tphandle = < 0x7 >;\n};\n")
        self.assertEqual(shape["empty"], {"uart0_default"})
        self.assertEqual(shape["cells"], {})
        self.assertEqual(shape["unfolded"], {})
        self.assertEqual(effective_cells(
            "uart0_default: uart0_default {\n agm,pins;\n};\n"), {})

    def test_cpp_expansion_without_dtc_is_unfolded(self):
        # zephyr.dts.pre: cpp expanded the macro, dtc has not folded it yet
        text = ("uart0_default: uart0_default {\n agm,pins = "
                "<((((6) & 0xf) << 0) | (((1) & 0xf) << 4) | (((0) & 0x3) "
                "<< 8))>;\n};\n")
        shape = pins_shape(text)
        self.assertEqual(shape["cells"], {})
        self.assertEqual(list(shape["unfolded"]), ["uart0_default"])

    def test_inline_comment_and_label_asymmetry(self):
        text = ("my_label: uart0_default {\n"
                "  /* a } brace and a \"string\" must not end the node */\n"
                "  agm,pins = < 0x176 >, /* RX */ < 0x101 >;\n};")
        self.assertEqual(effective_cells(text)["my_label"],
                         [(6, 7, 1), (1, 0, 1)])


class TestAfTable(unittest.TestCase):
    def test_repo_table_matches_the_dts_states(self):
        af = load_af(AF)
        self.assertEqual(len(af), 114)
        with open(DEFAULT_DTSI, encoding="utf-8") as f:
            text = f.read()
        named = {spec["state"] for spec in af.values() if "state" in spec}
        self.assertEqual(named - declared_states(text), set())

    def _load_with(self, body):
        with tempfile.NamedTemporaryFile("w", suffix=".yaml",
                                         delete=False) as f:
            f.write(body)
            path = f.name
        self.addCleanup(os.unlink, path)
        return load_af(path)

    def test_out_of_range_bank_and_bit_are_rejected(self):
        for spec in ("{ bank: 10, bit: 1, sdk: in }",
                     "{ bank: 1, bit: 8, sdk: in }"):
            with self.assertRaises(ValueError):
                self._load_with(f"af_pins:\n  X: {spec}\n")

    def test_duplicate_key_is_rejected(self):
        with self.assertRaises(ValueError) as ctx:
            self._load_with("af_pins:\n"
                            "  SPI0_SCK: { bank: 4, bit: 5, sdk: out }\n"
                            "  SPI0_SCK: { bank: 4, bit: 6, sdk: out }\n")
        self.assertIn("duplicate", str(ctx.exception))

    def test_vendor_header_comparison_catches_a_changed_row(self):
        # A mock AltaRiscv.h (three consecutive defines per function) with a
        # deliberately wrong MAC0_MDC row: the completeness gate has to say
        # which side disagrees, not just "tables differ".
        with tempfile.TemporaryDirectory() as tmp:
            header = os.path.join(tmp, "AltaRiscv.h")
            with open(header, "w", encoding="utf-8") as f:
                f.write("#define MAC0_MDC_AF_GPIO 4\n"
                        "#define MAC0_MDC_AF_GPIO_MASK (0x1 << 5)\n"
                        "#define MAC0_MDC_AF_GPIO_OUTPUT\n")
            res = run_tool("check_af_table.py", "--af", AF,
                           "--sdk-header", header)
        self.assertEqual(res.returncode, 8, res.stderr)
        self.assertIn("MAC0_MDC: table says bank 9 bit 7 out, header says "
                      "bank 4 bit 5 out", res.stderr)


class TestNetlist(unittest.TestCase):
    def test_multiline_assigns_are_parsed(self):
        # Review H3 claimed the regexes are line-anchored and miss a
        # wrapped assign; they are not (every separator is \\s*).
        func_to_pin, _ = parse_netlist(
            "assign MAC0_MDC =\n"
            "    PIN_58_out_en ? PIN_58_out_data : 1'bz;\n"
            "assign PIN_57_in =\n"
            "    MAC0_TX_CLK;\n")
        self.assertEqual(func_to_pin, {"MAC0_MDC": 58, "MAC0_TX_CLK": 57})

    def test_signal_fan_out_reaches_the_pin(self):
        _, pin_to_bits = parse_netlist(SPI_NETLIST)
        self.assertIn((4, 6), pin_to_bits[96])
        self.assertIn((0, 1), pin_to_bits[97])

    def test_case_c_route_is_resolved_through_board_ve(self):
        func_to_pin, _ = parse_netlist(SPI_NETLIST)
        case_c, name_to_pin = parse_ve(SPI_VE)
        self.assertEqual(case_c["SPI0_SI_IO0"], "si_io0")
        ve = route_views(case_c, name_to_pin, "board.ve (test)")
        self.assertEqual(resolve_route("SPI0_SI_IO0", func_to_pin, ve)[0], 92)
        # SPI0_SO_IO1 has no Case C row: board.ve is the only witness
        self.assertEqual(resolve_route("SPI0_SO_IO1", func_to_pin, ve)[0], 97)
        # ... and without a VE nothing can be resolved
        self.assertEqual(resolve_route("SPI0_SI_IO0", func_to_pin, None)[0],
                         None)

    def test_repeated_pin_in_one_concat_is_noted(self):
        # The vendor wrapper declares PIN_57 as both bit 5 and bit 6 of bank
        # 7. That tap is real, but it is not proof that the pin physically
        # feeds both -- say so instead of silently unioning the sets.
        notes = []
        net = ("wire [7:0] gpio7_io_in = {PIN_47_in, PIN_57_in, PIN_57_in, "
               "1'b0, PIN_38_in, 1'b0, 1'b0, 1'b0};\n")
        _, pin_to_bits = parse_netlist(net, notes.append)
        self.assertEqual(pin_to_bits[57], {(7, 5), (7, 6)})
        self.assertTrue(any("PIN_57 appears 2 times" in n for n in notes),
                        notes)


class TestBoardVe(unittest.TestCase):
    def test_case_a_row_that_repeats_a_case_c_function_is_noted(self):
        # The vendor example board's shape: the same function is a pin
        # (case A) *and* exposed to user logic (case C), and the signal is
        # the thing that actually reaches the pin.
        props = {
            "mcu-functions": ["SPI0_SI_IO0"],
            "mcu-pins": [92],
            "cpld-signals": ["si_io0"],
            "cpld-pins": [92],
            "cpld-directions": ["output"],
            "mcu-cpld-functions": ["SPI0_SI_IO0"],
            "mcu-cpld-signals": ["si_io0"],
        }
        errors, notes = check_bindings(props)
        self.assertEqual(errors, [])
        self.assertTrue(any("case C row" in n and "si_io0" in n
                            for n in notes), notes)

    def test_plain_case_a_and_case_b_stay_quiet(self):
        props = {
            "mcu-functions": ["SPI0_SI_IO0", "SPI0_SCK"],
            "mcu-pins": [92, 93],
            "cpld-signals": ["sspi0_mosi"],
            "cpld-pins": [92],
            "cpld-directions": ["input"],
        }
        errors, notes = check_bindings(props)
        self.assertEqual(errors, [])
        self.assertEqual([n for n in notes if "case C" in n], [])

    def test_read_back_pin_renders_the_input_direction(self):
        # The vendor reference VE spells the flash's MOSI/IO0 pin
        # `SPI0_SI_IO0 PIN_92:INPUT` -- the function's default direction is
        # the output one, so the row needs the extra direction to put the pin
        # back on SPI0's RX path (samples/spi_quad_read,:
        # without it the generated wrapper ties the MCU's read path to 1'b0).
        ve = gen.render_ve({
            "sysclk-frequency": 200_000_000,
            "hseclk-frequency": 8_000_000,
            "mcu-functions": ["SPI0_WPN_IO2", "SPI0_SO_IO1", "SPI0_SI_IO0"],
            "mcu-pins": [98, 97, 92],
            "agm,mcu-input-pins": [92],
        })
        self.assertIn("SPI0_SI_IO0 PIN_92:INPUT\n", ve)
        self.assertIn("SPI0_SO_IO1 PIN_97\n", ve)
        self.assertIn("SPI0_WPN_IO2 PIN_98\n", ve)

    def test_an_explicit_direction_wins_over_the_read_back_default(self):
        ve = gen.render_ve({
            "sysclk-frequency": 200_000_000,
            "hseclk-frequency": 8_000_000,
            "mcu-functions": ["SPI0_SI_IO0"],
            "mcu-pins": [92],
            "mcu-directions": ["inout"],
            "agm,mcu-input-pins": [92],
        })
        self.assertIn("SPI0_SI_IO0 PIN_92:INOUT\n", ve)

    def test_read_back_pin_needs_its_own_case_a_row(self):
        # A read-back pin that is not routed at all is a typo, not a no-op:
        # it would otherwise render nothing and leave the MCU's read path
        # tied to zero with no hint as to why.
        try:
            gen.render_ve({
                "sysclk-frequency": 200_000_000,
                "hseclk-frequency": 8_000_000,
                "mcu-functions": ["SPI0_WPN_IO2"],
                "mcu-pins": [98],
                "agm,mcu-input-pins": [92],
            })
        except ValueError as exc:
            self.assertIn("PIN_92", str(exc))
        else:
            raise AssertionError("an unrouted read-back pin must be refused")


class TestCommittedFragments(unittest.TestCase):
    """The committed fragments are build products, and re-deriving them from
    their source pin lists needs no Zephyr tree, no SDK and no build -- so
    "someone edited a pin list and did not re-run the generator" fails here,
    in CI. (build_bitstream.sh has the same check, but only for whoever
    builds.)"""

    def test_every_fragment_has_a_known_source(self):
        found = sorted(n for n in
                       os.listdir(os.path.join(REPO, FRAGMENT_DIR))
                       if n.startswith("pinctrl-") and n.endswith(".dtsi"))
        self.assertEqual(found, sorted(FRAGMENT_SOURCES),
                         "a fragment was added or removed: update "
                         "FRAGMENT_SOURCES in this file")

    def test_fragments_match_their_pin_lists(self):
        af = load_af(AF)
        for frag, sources in sorted(FRAGMENT_SOURCES.items()):
            got = fragment_cells(f"{FRAGMENT_DIR}/{frag}")
            self.assertTrue(got, f"{frag}: no agm,pins cells parsed")
            for src in sources:
                want = build(pin_list_functions(src), af, lambda msg: None)
                self.assertEqual(
                    got, want,
                    f"{frag} does not match {src} -- re-run "
                    f"tools/generate_pinctrl_dtsi.py on a build of that "
                    f"sample")


class TestStateContent(unittest.TestCase):
    def test_bidir_row_order_is_checked(self):
        # The AF table's `bidir` emits OUT then IN, and the last cell of a
        # row is the DIR the pin is left with (pinctrl_configure_pins walks
        # the cells in order), so a reversed pair is not equivalent.
        af = {"MAC0_MDIO": {"bank": 4, "bit": 0, "sdk": "inout",
                            "dir": "bidir", "state": "eth0_default"}}
        want = [(4, 0, 1), (4, 0, 0)]
        self.assertEqual(compare_state("eth0_default", want, want, af), [])
        problems = compare_state("eth0_default", [(4, 0, 0), (4, 0, 1)],
                                 want, af)
        self.assertTrue(any("decides the DIR" in p for p in problems),
                        problems)

    def test_bidir_pair_must_carry_two_different_directions(self):
        # L6's other half: the two cells of a row have to differ, and a
        # pair that repeats one direction is a missing cell, not a pass.
        af = {"MAC0_MDIO": {"bank": 4, "bit": 0, "sdk": "inout",
                            "dir": "bidir", "state": "eth0_default"}}
        want = [(4, 0, 1), (4, 0, 0)]
        problems = compare_state("eth0_default", [(4, 0, 1), (4, 0, 1)],
                                 want, af)
        self.assertTrue(any("missing cells" in p for p in problems), problems)


NETLIST_FIXTURE = os.path.join(FIXTURES, "example_board.v")
VE_FIXTURE = os.path.join(FIXTURES, "example_board.ve")


class TestRealNetlistFixture(unittest.TestCase):
    """Parser coverage against a full-size netlist + pin map.

    The pair that used to live in tools/tests/fixtures/ was generated from the
    vendor reference design and is not redistributed with this repository, so
    the three cases below run only when a copy is dropped into that directory
    (same names). What they cover -- multi-line
    assigns, the `gpio*_io_in` concatenations, CPLD signals spelled instead of
    pins, and the two real "claimed but not wired" pins -- has no synthetic
    stand-in yet.
    """

    @classmethod
    def setUpClass(cls):
        if not (os.path.exists(NETLIST_FIXTURE) and os.path.exists(VE_FIXTURE)):
            raise unittest.SkipTest(
                "example_board.{v,ve} not present (not redistributed)")
        with open(NETLIST_FIXTURE, encoding="utf-8", errors="replace") as f:
            cls.netlist = f.read()

    def test_functions_and_cpld_signals_reach_pins(self):
        func_to_pin, pin_to_bits = parse_netlist(self.netlist)
        # MAC0 RMII rows and the UART console, straight from the wrapper...
        self.assertEqual(func_to_pin["MAC0_RXD0"], 47)
        self.assertEqual(func_to_pin["MAC0_TX_CLK"], 57)
        self.assertEqual(func_to_pin["MAC0_MDC"], 58)
        self.assertEqual(func_to_pin["MAC0_MDIO"], 59)
        self.assertEqual(func_to_pin["UART0_UARTRXD"], 69)
        # ... and the CPLD signals of the SPI0 flash route, which is all the
        # wrapper ever shows of Case C.
        self.assertEqual(func_to_pin["si_io0"], 92)
        self.assertEqual(func_to_pin["so_io1"], 97)
        self.assertEqual(pin_to_bits[92], {(0, 0)})
        self.assertEqual(pin_to_bits[97], {(0, 1)})
        # PIN_57 is listed twice in gpio7_io_in (bits 5 and 6).
        self.assertEqual(pin_to_bits[57], {(7, 5), (7, 6)})

    def test_the_pins_board_ve_claims_but_the_wrapper_never_wires(self):
        # The ve says csn PIN_96 and
        # sck PIN_93, the netlist wires neither. Pin the fact here so the
        # "not wired" failure keeps meaning what it says.
        _, pin_to_bits = parse_netlist(self.netlist)
        self.assertNotIn(93, pin_to_bits)
        self.assertNotIn(96, pin_to_bits)

    def test_repeated_pin_is_noted_on_real_input(self):
        notes = []
        parse_netlist(self.netlist, notes.append)
        self.assertTrue(any("PIN_57 appears 2 times" in n for n in notes),
                        notes)

    def test_cli_against_the_fixture_tracks_the_two_real_failures(self):
        # The SPI0 pin list whose pins come from this fixture's own ve:
        # SI_IO0 (Case C) and SO_IO1 resolve, CSN and SCK do not, because
        # their pins are not wired.
        with tempfile.TemporaryDirectory() as tmp:
            dts = os.path.join(tmp, "spi0.dts")
            with open(dts, "w", encoding="utf-8") as f:
                f.write(dts_with_pin_list(
                    SPI_FNS, SPI_PADS,
                    f"spi0_default: spi0_default {{\n agm,pins = "
                    f"{SPI_CELLS};\n}};\n"))
            res = run_tool("check_pinctrl.py", "--dts", dts,
                           "--netlist", NETLIST_FIXTURE)
        self.assertEqual(res.returncode, 7, res.stderr)
        self.assertIn("Case C", res.stderr)          # SI_IO0 was resolved
        self.assertIn("PIN_93 is not wired", res.stderr)
        self.assertIn("PIN_96 is not wired", res.stderr)


class TestCli(unittest.TestCase):
    """End-to-end behaviour of the two CLIs, including exit codes."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.tmp = self.tmp.name

    def write(self, name, text):
        path = os.path.join(self.tmp, name)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        return path

    def test_generate_check_rejects_an_unfolded_input(self):
        dts = self.write("pre.dts", dts_with_pin_list(
            SPI_FNS, SPI_PADS,
            "spi0_default: spi0_default {\n agm,pins = "
            "<((((0) & 0xf) << 0))>;\n};\n"))
        res = run_tool("generate_pinctrl_dtsi.py", "--dts", dts, "--check")
        self.assertEqual(res.returncode, 5, res.stderr)
        self.assertIn("un-folded", res.stderr)

    def test_generate_check_flags_a_missing_include(self):
        dts = self.write("empty.dts", dts_with_pin_list(
            SPI_FNS, SPI_PADS,
            "spi0_default: spi0_default {\n agm,pins = <>;\n};\n"))
        res = run_tool("generate_pinctrl_dtsi.py", "--dts", dts, "--check")
        self.assertEqual(res.returncode, 6, res.stderr)
        self.assertIn("holds no cells", res.stderr)
        self.assertIn("hint: regenerate", res.stderr)

    def test_generate_check_accepts_the_matching_state(self):
        dts = self.write("ok.dts", dts_with_pin_list(
            SPI_FNS, SPI_PADS,
            f"spi0_default: spi0_default {{\n agm,pins = {SPI_CELLS};\n}};\n"))
        res = run_tool("generate_pinctrl_dtsi.py", "--dts", dts, "--check")
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_check_pinctrl_passes_with_case_c_route(self):
        dts = self.write("ok.dts", dts_with_pin_list(
            SPI_FNS, SPI_PADS,
            f"spi0_default: spi0_default {{\n agm,pins = {SPI_CELLS};\n}};\n"))
        net = self.write("board.v", SPI_NETLIST)
        self.write("board.ve", SPI_VE)
        res = run_tool("check_pinctrl.py", "--dts", dts, "--netlist", net)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("Case C", res.stderr)      # the note, not a failure

    def test_check_pinctrl_fails_on_a_pin_the_netlist_does_not_wire(self):
        dts = self.write("ok.dts", dts_with_pin_list(
            SPI_FNS, SPI_PADS,
            f"spi0_default: spi0_default {{\n agm,pins = {SPI_CELLS};\n}};\n"))
        net = self.write("board.v", SPI_NETLIST_UNWIRED)
        self.write("board.ve", SPI_VE)
        res = run_tool("check_pinctrl.py", "--dts", dts, "--netlist", net)
        self.assertEqual(res.returncode, 7, res.stderr)
        self.assertIn("not wired to any GPIO bit", res.stderr)

    def test_check_pinctrl_fails_when_the_bitstream_uses_another_pin(self):
        dts = self.write("ok.dts", dts_with_pin_list(
            SPI_FNS, SPI_PADS,
            f"spi0_default: spi0_default {{\n agm,pins = {SPI_CELLS};\n}};\n"))
        net = self.write("board.v", SPI_NETLIST.replace(
            "PIN_93_out_en", "PIN_99_out_en"))
        self.write("board.ve", SPI_VE)
        res = run_tool("check_pinctrl.py", "--dts", dts, "--netlist", net)
        self.assertEqual(res.returncode, 7, res.stderr)
        self.assertIn("netlist says PIN_99", res.stderr)

    def test_check_pinctrl_rejects_an_unfolded_input_too(self):
        dts = self.write("pre.dts", dts_with_pin_list(
            SPI_FNS, SPI_PADS,
            "spi0_default: spi0_default {\n agm,pins = <AGM_PINCTRL(0, 0, "
            "AGM_PINCTRL_OUTPUT)>;\n};\n"))
        res = run_tool("check_pinctrl.py", "--dts", dts)
        self.assertEqual(res.returncode, 5, res.stderr)
        self.assertIn("un-folded", res.stderr)

    def test_post_route_netlist_is_named_as_the_likely_wrong_input(self):
        # L9's second half: a post-route file has none of the wrapper's nets.
        dts = self.write("ok.dts", dts_with_pin_list(
            SPI_FNS, SPI_PADS,
            f"spi0_default: spi0_default {{\n agm,pins = {SPI_CELLS};\n}};\n"))
        net = self.write("routed.v", "module example_board (a, b);\n"
                                     "assign a = b;\nendmodule\n")
        res = run_tool("check_pinctrl.py", "--dts", dts, "--netlist", net)
        self.assertEqual(res.returncode, 1, res.stderr)
        self.assertIn("post-route", res.stderr)
        self.assertIn("prepare logic", res.stderr)

    def test_bidir_row_is_explained_in_a_note(self):
        # The MDIO row of a MAC board: the note says what the two cells are
        # and that their order is part of the contract.
        dts = self.write("mdio.dts", dts_with_pin_list(
            ["MAC0_MDIO"], [59],
            "eth0_default: eth0_default {\n"
            "\tagm,pins = < 0x104 >, < 0x4 >;\n};\n"))
        res = run_tool("check_pinctrl.py", "--dts", dts)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("bidir row", res.stderr)
        self.assertIn("output/input", res.stderr)

    def test_case_c_alias_can_come_from_the_pin_list(self):
        # No board.ve next to this netlist (different stem), so the only
        # place the SPI0_SI_IO0 <-> si_io0 alias can come from is the pins
        # node's own mcu-cpld-* rows.
        dts = self.write("aliased.dts", """
/ {
\tagrv2k_pins: agrv2k-pins {
\t\tcompatible = "agm,agrv2k-pins";
\t\tmcu-functions = "SPI0_SI_IO0";
\t\tmcu-pins = < 92 >;
\t\tmcu-cpld-functions = "SPI0_SI_IO0";
\t\tmcu-cpld-signals = "si_io0";
\t};
};
spi0_default: spi0_default {
\tagm,pins = < 0x100 >;
};
""")
        net = self.write("bare.v", SPI_NETLIST)
        res = run_tool("check_pinctrl.py", "--dts", dts, "--netlist", net)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("mcu-cpld-functions", res.stderr)

        # --no-ve means "only what the netlist proves": the aliased function
        # is then unresolvable and reported instead of quietly accepted.
        res = run_tool("check_pinctrl.py", "--dts", dts, "--netlist", net,
                       "--no-ve")
        self.assertEqual(res.returncode, 7, res.stderr)
        self.assertIn("does not route this function at all", res.stderr)

    def test_mirror_guard_refuses_a_west_workspace_path(self):
        dts = self.write("ok.dts", dts_with_pin_list(
            SPI_FNS, SPI_PADS,
            f"spi0_default: spi0_default {{\n agm,pins = {SPI_CELLS};\n}};\n"))
        out = os.path.join(self.tmp, "ws", "modules", "hal_ag32",
                           "dts", "riscv", "agm", "pinctrl-x.dtsi")
        os.makedirs(os.path.dirname(out))
        res = run_tool("generate_pinctrl_dtsi.py", "--dts", dts, "--out", out)
        self.assertEqual(res.returncode, 7, res.stderr)
        self.assertIn("mirror", res.stderr)
        self.assertFalse(os.path.exists(out))


if __name__ == "__main__":
    unittest.main()
