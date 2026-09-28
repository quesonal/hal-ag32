# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/check_bitstream_clock.py — the fabric/firmware clock gate.

The tool only ever compares numbers that the fabric's own build inputs state,
so the tests are about the three ways those numbers can arrive (.ve text, the
generated Verilog's PLL parameters, or a merged devicetree) and about the
verdicts: agree, mismatch, or "nothing to compare".
"""

import importlib.util
import pathlib
import sys

TOOLS = pathlib.Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


chk = load("check_bitstream_clock")

VE_200 = "SYSCLK 200\nBUSCLK 100\nHSECLK 8\n\nGPIO4_1 PIN_51\n"


def verilog(hse="8.0", fb=149, high=2, low=2, bypass=0):
    return (f'defparam pll_inst.CLKIN_FREQ      = "{hse}";\n'
            f"defparam pll_inst.CLKFB_HIGH      = 8'd{fb};\n"
            f"defparam pll_inst.CLKOUT0_HIGH    = 8'd{high};\n"
            f"defparam pll_inst.CLKOUT0_LOW     = 8'd{low};\n"
            f"defparam pll_inst.CLKOUT0_BYPASS  = 1'b{bypass};\n")


def config(hz):
    return (f"CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC={hz}\n"
            "CONFIG_RISCV_MACHINE_TIMER_SYSTEM_CLOCK_DIVIDER=0\n")


# The merged zephyr.dts is dtc output: hex, and spaces inside the cells.
DTS = ('\t\t\tflash-max-frequency = < 0x5f5e100 >; /* 100 MHz */\n'
       '\t\thseclk-frequency = < 0x7a1200 >;  /* 8 MHz */\n')


def write(tmp_path, name, text):
    p = tmp_path / name
    p.write_text(text)
    return str(p)


def test_ve_states_the_clock():
    assert chk.parse_ve(VE_200) == {"SYSCLK": 200_000_000,
                                    "BUSCLK": 100_000_000,
                                    "HSECLK": 8_000_000}


def test_verilog_pll_parameters_give_the_sysclk():
    """The two dev board bitstreams, cross-checked against their own .ve files."""
    assert chk.parse_verilog(verilog(fb=149))["SYSCLK"] == 200_000_000
    assert chk.parse_verilog(verilog(fb=74))["SYSCLK"] == 100_000_000
    assert chk.parse_verilog(verilog(fb=74))["HSECLK"] == 8_000_000
    # A bypassed PLL is not a statement about SYSCLK at all.
    assert chk.parse_verilog(verilog(bypass=1)) == {}
    # ... and neither is a file that is not the generated board.v.
    assert chk.parse_verilog("module top; endmodule\n") == {}


def test_matching_bitstream_passes(tmp_path, capsys):
    bs = write(tmp_path, "b.bin", "")
    write(tmp_path, "b.ve", VE_200)
    rc = chk.main([bs, "--config", write(tmp_path, "c.config", config(200_000_000)),
                   "--dts", write(tmp_path, "z.dts", DTS)])
    out = capsys.readouterr().out

    assert rc == 0
    assert "SYSCLK" in out and "HSECLK" in out and "BUSCLK" in out
    assert "MISMATCH" not in out


def test_mismatched_bitstream_is_refused(tmp_path, capsys):
    """100 MHz fabric + 200 MHz firmware: the 3.31 drift, caught up front."""
    bs = write(tmp_path, "b.bin", "")
    write(tmp_path, "b.ve", "SYSCLK 100\nBUSCLK 50\nHSECLK 8\n")
    rc = chk.main([bs, "--config", write(tmp_path, "c.config", config(200_000_000))])
    err = capsys.readouterr().err

    assert rc == 1
    assert "REFUSING" in err and "100000000" in err and "200000000" in err


def test_verilog_fallback_when_there_is_no_ve(tmp_path, capsys):
    bs = write(tmp_path, "b.bin", "")
    write(tmp_path, "b.v", verilog(fb=74))
    rc = chk.main([bs, "--sysclk-hz", "100000000"])

    assert rc == 0
    assert "(verilog)" in capsys.readouterr().out


def test_unverifiable_is_a_warning_unless_required(tmp_path, capsys):
    bs = write(tmp_path, "b.bin", "")

    assert chk.main([bs, "--sysclk-hz", "200000000"]) == 0
    assert "unverified" in capsys.readouterr().out
    assert chk.main([bs, "--sysclk-hz", "200000000", "--require-ve"]) == 2
    assert "cannot be checked" in capsys.readouterr().err


def test_a_config_without_the_constant_is_a_check_error(tmp_path, capsys):
    bs = write(tmp_path, "b.bin", "")
    write(tmp_path, "b.ve", VE_200)
    cfg = write(tmp_path, "c.config", "# nothing here\n")

    assert chk.main([bs, "--config", cfg]) == 2
    assert "CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC" in capsys.readouterr().err


def test_dts_hex_cells_are_parsed(tmp_path, capsys):
    bs = write(tmp_path, "b.bin", "")
    write(tmp_path, "b.ve", "SYSCLK 200\nHSECLK 25\n")
    rc = chk.main([bs, "--dts", write(tmp_path, "z.dts", DTS)])

    # Only HSECLK can be compared (the dts states no SYSCLK) -- and 25 MHz does
    # not match the dts's 8 MHz.
    assert rc == 1
    assert "HSECLK" in capsys.readouterr().err
