#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Generate the Quartus-ready AgRV2K FPGA logic directory from board.ve.
#
# This script emits:
#   logic/board.vx     Verilog netlist (Quartus input)
#   logic/board.hx     header (defines / parameters)
#   logic/board.vex    pre-synthesis constraints file
#   logic/board.asf    Supra project skeleton
#   logic/board.qsf    Quartus settings file (open in Quartus GUI or
#                      cd <build>/logic && quartus_sh -t af_quartus.tcl)
#   logic/af_quartus.tcl  Quartus TCL script (copied from SDK etc/)
#   logic/agrv_fpga_decomp.inc  (staged for SDK check_logic at flash)
#
# The actual FPGA synthesis (Quartus) and bitstream compile (Supra
# gen_logic.tcl reading the .vqm) MUST run on a Quartus workstation.
# Quartus's part (simulation/modelsim/<design>.vo) comes back into
# <logic_dir>, and tools/compile_bitstream.sh then runs Supra here to write
# <logic_dir>/board.bin. $AGM_BITSTREAM_BIN is only where `west flash` looks
# for a bitstream to write to the board's fabric address.
#
# Usage:
#   build_bitstream.sh <board_dir> <logic_dir>
#
# Arguments:
#   board_dir   directory containing the per-board .dts (board.ve is
#               generated from the preprocessed dts automatically)
#   logic_dir   output directory (created if missing)
#
# Environment overrides:
#   AGRV_SDK_PATH           root of the AgRV PlatformIO install
#                           default: $HOME/AgRV_pio
#   SUPRA_HOME              directory containing the af_cmd binary
#                           default: $AGRV_SDK_PATH/packages/tool-agrv_logic
#   AGRV_PLATFORM_ETC       directory with gen_vlog, pre_logic.tcl
#                           default: $AGRV_SDK_PATH/platforms/AgRV/etc
#   AGM_LOGIC_DEVICE        package the bitstream is compiled for
#                           (AGRV2KL100 / L100H / L64 / L64H / L48 /
#                           Q32). Default: the `agm,logic-device`
#                           property of the pins node in the dts, or
#                           AGRV2KL100 when no dts is available.
#   AGM_LOGIC_TOPPIN        true/false          default: false
#   AGM_IP_DIR              directory with IP *.vx (default: empty)
#   AGM_DTS                  path to a Zephyr-preprocessed dts
#                           (${PROJECT_BINARY_DIR}/zephyr.dts), or the
#                           literal "auto" to build the dts here from
#                           <board_dir>/*.dts with cpp+dtc -- no Zephyr
#                           build required. Unset means "use the board.ve
#                           already on disk".
#   AGM_BOARD_DTS           board .dts to use with AGM_DTS=auto (default:
#                           the single <board_dir>/*.dts).
#   AGM_DTS_OVERLAY         space-separated extra dts snippets appended to
#                           the board .dts in auto mode (a sample's
#                           boards/<board>.overlay, say).
#   AGM_SKIP_BOARD_VE_GEN   if set non-empty, skip the dts→board.ve
#                           step (use when board.ve is already on disk,
#                           e.g. CI cache hydration)
#   AGM_ALLOW_PIN_CONFLICTS if set non-empty, let generate_board_ve.py
#                           render a pin map with ambiguous pins (duplicate
#                           bindings become warnings instead of a hard
#                           error). Only for knowingly hand-tuned maps.
#   AGM_USER_RTL            space-separated list of Verilog files of your own
#                           logic (an IP the pin map already describes, e.g.
#                           the full-duplex SPI wrapper). They are copied into
#                           <logic_dir> and registered in the Quartus project
#                           via pre_logic.tcl's IP_VV -- the same slot the
#                           vendor's `ip_name` flow uses -- and gen_vlog is
#                           called with `-m <ip>.v`, which is what emits the
#                           instance in board.v.
#   AGM_LOGIC_IP            the IP's file basename inside the project (the
#                           sample's CMake resolves <app>/ip/<name>.v to
#                           AGM_USER_RTL). Defaults to the basename of the
#                           first AGM_USER_RTL file.
#   AGM_LOGIC_IP_FLOW=off   skip the IP prepare step (Step 0b) and pass only
#                           `-m <ip>.v` to gen_vlog.

set -eu

# --- venv guard  ----
# gen_vlog / generate_board_ve.py need the venv's `devicetree` module; a bare
# `python3` silently resolves to /usr/bin/python3. See tools/flash_fw.sh.
if [ -z "${VIRTUAL_ENV:-}" ]; then
	echo "FATAL: no Zephyr virtualenv active - activate the repo venv first" >&2
	exit 3
fi
PYTHON=${PYTHON:-$VIRTUAL_ENV/bin/python3}

if [ $# -ne 2 ]; then
	echo "Usage: $0 <board_dir> <logic_dir>" >&2
	echo "  board_dir: directory containing dist/<board>.dist" >&2
	echo "  logic_dir: output directory (will be created)" >&2
	exit 1
fi

BOARD_DIR=$1
LOGIC_DIR=$2

# gen_vlog runs after a `cd "$LOGIC_DIR"`, so a relative BOARD_DIR (or
# AGM_DTS) would resolve against the wrong directory from there. Resolve
# both to absolute paths now.
BOARD_DIR=$(cd "$BOARD_DIR" && pwd)
mkdir -p "$LOGIC_DIR"
LOGIC_DIR=$(cd "$LOGIC_DIR" && pwd)

# --- Locate toolchain ------------------------------------------------
AGRV_SDK_PATH=${AGRV_SDK_PATH:-$HOME/AgRV_pio}
SUPRA_HOME=${SUPRA_HOME:-$AGRV_SDK_PATH/packages/tool-agrv_logic}
AGRV_PLATFORM_ETC=${AGRV_PLATFORM_ETC:-$AGRV_SDK_PATH/platforms/AgRV/etc}

AF_CMD=$SUPRA_HOME/bin/af_cmd
PRE_LOGIC=$AGRV_PLATFORM_ETC/pre_logic.tcl
GEN_VLOG=$AGRV_PLATFORM_ETC/gen_vlog

# Resolved after step 0: the dts may carry `agm,logic-device`, and the
# environment (AGM_LOGIC_DEVICE) overrides that either way.
LOGIC_DEVICE=${AGM_LOGIC_DEVICE:-}
# LOGIC_DESIGN must match the basename of the .hx / .vx files emitted
# by gen_vlog (pre_logic.tcl hard-codes "${LOGIC_DESIGN}.hx"). Default
# to "board" so a single board.ve always produces board.{hx,vx,qsf}.
LOGIC_DESIGN=${AGM_LOGIC_DESIGN:-board}
LOGIC_MODULE=${AGM_LOGIC_MODULE:-$LOGIC_DESIGN}
LOGIC_TOPPIN=${AGM_LOGIC_TOPPIN:-false}

# Tools dir (this script lives in <hal_ag32>/tools/) — used to locate
# generate_board_ve.py for the dist→board.ve step.
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

if [ ! -x "$AF_CMD" ]; then
	echo "Error: af_cmd not found at $AF_CMD" >&2
	echo "Set SUPRA_HOME to the tool-agrv_logic package directory." >&2
	exit 2
fi
if [ ! -x "$GEN_VLOG" ]; then
	echo "Error: gen_vlog not found at $GEN_VLOG" >&2
	echo "Set AGRV_PLATFORM_ETC to the platforms/AgRV/etc directory." >&2
	exit 3
fi

# --- Step 0: render board.ve from preprocessed dts ------------------
#
# The AgRV2K bitstream pinout is expressed in dts as the
# `agm,agrv2k-pins` node — see dts/riscv/agm/agrv2k-pins.dtsi
# (shared) + each board.dts (overrides SYSCLK/HSECLK). Zephyr's
# devicetree.cmake preprocesses all includes into
# ${PROJECT_BINARY_DIR}/zephyr.dts; we feed that into
# generate_board_ve.py to emit board.ve.
#
# Skip this step if:
#   - AGM_SKIP_BOARD_VE_GEN is non-empty (CI fast path / manually
#     edited board.ve on disk).
#   - AGM_DTS is not set (legacy invocation — use existing board.ve
#     on disk if present).
DEVICE_DTS=""
if [ -n "${AGM_SKIP_BOARD_VE_GEN:-}" ]; then
	echo ">>> [0/2] AGM_SKIP_BOARD_VE_GEN set — using existing board.ve"
elif [ "${AGM_DTS:-}" = "auto" ]; then
	# dtsi → board.ve without a Zephyr cmake build: run the same
	# preprocessor + dtc Zephyr would run, but on the board's own .dts
	# (plus any overlays in AGM_DTS_OVERLAY). The board.ve this produces
	# is byte-identical to the one a full Zephyr
	# build renders through ${PROJECT_BINARY_DIR}/zephyr.dts.
	ZB=${ZEPHYR_BASE:-}
	if [ -z "$ZB" ]; then
		# Ask west from wherever this runs (the workspace root is not
		# necessarily the module's parent), and fall back to the sibling
		# layout west creates under <workspace>/modules/hal_ag32.
		ZB=$(west list -f '{abspath}' zephyr 2>/dev/null || true)
		if [ -z "$ZB" ] && [ -d "$SCRIPT_DIR/../../../zephyr" ]; then
			ZB=$(cd "$SCRIPT_DIR/../../../zephyr" && pwd)
		fi
	fi
	if [ -z "$ZB" ] || [ ! -d "$ZB/dts" ]; then
		echo "Error: AGM_DTS=auto needs the Zephyr tree; set ZEPHYR_BASE or run inside the west workspace" >&2
		exit 4
	fi

	BOARD_DTS=${AGM_BOARD_DTS:-$(ls "$BOARD_DIR"/*.dts 2>/dev/null | head -1)}
	if [ -z "$BOARD_DTS" ] || [ ! -f "$BOARD_DTS" ]; then
		echo "Error: AGM_DTS=auto found no <board_dir>/*.dts (set AGM_BOARD_DTS)" >&2
		exit 4
	fi

	MODULE_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
	DTS_TMP=$(mktemp -d)
	# `find -delete` rather than rm -rf: some sandboxes reject the latter
	# outright, and this is a plain mktemp tree.
	trap 'find "$DTS_TMP" -depth -delete 2>/dev/null || true' EXIT

	# shellcheck disable=SC2086 # AGM_DTS_OVERLAY is a deliberate list
	cat "$BOARD_DTS" ${AGM_DTS_OVERLAY:-} > "$DTS_TMP/board.dts"
	echo ">>> [0/2] dts→dts: $BOARD_DTS${AGM_DTS_OVERLAY:+ + $AGM_DTS_OVERLAY}"
	cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
		-I "$MODULE_ROOT/dts" -I "$MODULE_ROOT/dts/riscv" -I "$MODULE_ROOT/include" \
		-I "$ZB/include" -I "$ZB/dts" -I "$ZB/dts/common" \
		"$DTS_TMP/board.dts" > "$DTS_TMP/board.pre.dts"
	dtc -I dts -O dts -o "$DTS_TMP/board.dts.out" \
		-Wno-simple_bus_reg "$DTS_TMP/board.pre.dts" 2>/dev/null

	echo ">>> [0/2] generate_board_ve.py: $DTS_TMP/board.dts.out → $LOGIC_DIR/board.ve"
	"$PYTHON" "$SCRIPT_DIR/generate_board_ve.py" \
		--dts "$DTS_TMP/board.dts.out" \
		--board-dir "$BOARD_DIR" \
		--out-dir "$LOGIC_DIR"
	DEVICE_DTS="$DTS_TMP/board.dts.out"
elif [ -n "${AGM_DTS:-}" ]; then
	echo ">>> [0/2] generate_board_ve.py: $AGM_DTS → $LOGIC_DIR/board.ve"
	"$PYTHON" "$SCRIPT_DIR/generate_board_ve.py" \
		--dts "$AGM_DTS" \
		--board-dir "$BOARD_DIR" \
		--out-dir "$LOGIC_DIR"
	DEVICE_DTS="$AGM_DTS"
else
	echo ">>> [0/2] AGM_DTS not set — using existing $LOGIC_DIR/board.ve if present"
fi

if [ ! -f "$LOGIC_DIR/board.ve" ]; then
	echo "Error: board.ve not found in $LOGIC_DIR and no AGM_DTS to render from" >&2
	exit 4
fi

# --- Step 0.4: the committed pinctrl fragment must match the pin list ---
#
# dts/riscv/agm/pinctrl-{default,devmac}.dtsi are generated from this same
# pins node's mcu-functions/mcu-pins and committed.
# A pin-list edit that forgot to re-run the generator is invisible to
# every other gate here: the bitstream and the firmware are rendered from the
# same text, so only the AFSEL bank/bit each driver programs would disagree --
# at run time, on a board. The firmware side has twister; this is the
# bitstream side's equivalent.
if [ -n "$DEVICE_DTS" ]; then
	echo ">>> [0/2] generate_pinctrl_dtsi.py --check: $DEVICE_DTS"
	if ! "$PYTHON" "$SCRIPT_DIR/generate_pinctrl_dtsi.py" \
		--dts "$DEVICE_DTS" --check; then
		echo "Error: the pinctrl state(s) above do not match this pin list." >&2
		echo "       Regenerate the fragment from this same dts:" >&2
		echo "         $PYTHON $SCRIPT_DIR/generate_pinctrl_dtsi.py \\" >&2
		echo "             --dts $DEVICE_DTS --out <fragment>.dtsi" >&2
		echo "       the board.ve layout does not match the pins." >&2
		exit 6
	fi
fi

# --- Step 0.5: the package decides which pins exist --------------------
#
# Different packages (封装) expose different pins, and therefore
# different peripherals: AGRV2KL100 has 79 user IO pins, AGRV2KQ32 has
# 26. `agm,logic-device` in the pins node is the board's declaration of
# the package this pin map targets; take it as the default for gen_vlog's
# -d and let AGM_LOGIC_DEVICE override.
#
# Then check every pin in board.ve against that device's pin list, because
# gen_vlog does NOT fail on a mismatch: given board.ve with L100 pins and
# `-d AGRV2KL64` it prints
#     Warn: IP pin PIN_97 is ignored because no IP macro is specified
# once per pin and exits 0 (11 pins). A wrong package
# would therefore yield a bitstream that silently leaves those pins
# unconnected -- the MCU peripheral is configured, the pin is not wired.
if [ -z "$LOGIC_DEVICE" ] && [ -n "$DEVICE_DTS" ]; then
	LOGIC_DEVICE=$(sed -n 's/.*agm,logic-device[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' \
		"$DEVICE_DTS" | head -1)
	if [ -n "$LOGIC_DEVICE" ]; then
		echo ">>> [0/2] logic device from dts: $LOGIC_DEVICE"
	fi
fi
if [ -z "$LOGIC_DEVICE" ]; then
	LOGIC_DEVICE=AGRV2KL100
	echo ">>> [0/2] logic device defaulted to $LOGIC_DEVICE (set AGM_LOGIC_DEVICE)" >&2
fi

_agm_dev_pins=$(mktemp)
_agm_ve_pins=$(mktemp)
"$PYTHON" "$GEN_VLOG" -p -d "$LOGIC_DEVICE" 2>/dev/null \
	| grep -o 'PIN_[0-9]*' | sort -u > "$_agm_dev_pins" || true
grep -o 'PIN_[0-9]*' "$LOGIC_DIR/board.ve" | sort -u > "$_agm_ve_pins" || true
if [ ! -s "$_agm_dev_pins" ]; then
	find "$_agm_dev_pins" "$_agm_ve_pins" -delete 2>/dev/null || true
	echo "Error: gen_vlog does not know device '$LOGIC_DEVICE'." >&2
	echo "       Known devices: AGRV2KL100 AGRV2KL100H AGRV2KL64 AGRV2KL64H AGRV2KL48 AGRV2KQ32" >&2
	exit 5
fi
_agm_outside=$(comm -13 "$_agm_dev_pins" "$_agm_ve_pins")
_agm_ve_count=$(wc -l < "$_agm_ve_pins")
find "$_agm_dev_pins" "$_agm_ve_pins" -delete 2>/dev/null || true
if [ -n "$_agm_outside" ]; then
	_n=$(printf '%s\n' "$_agm_outside" | wc -l)
	echo "Error: $LOGIC_DIR/board.ve names $_n pin(s) that $LOGIC_DEVICE does not have:" >&2
	echo "       $(printf '%s' "$_agm_outside" | tr '\n' ' ')" >&2
	echo "       Those pins belong to a bigger package. Fix mcu-pins/cpld-pins in the pin list," >&2
	echo "       or set the right package: change 'agm,logic-device' in the pins node or" >&2
	echo "       export AGM_LOGIC_DEVICE. Not checking would be silent: gen_vlog only warns" >&2
	echo "       ('IP pin PIN_x is ignored because no IP macro is specified') and exits 0." >&2
	exit 5
fi
echo ">>> [0/2] package check: $_agm_ve_count pin(s), all present on $LOGIC_DEVICE"

# --- Step 1: gen_vlog → board.v + board.hx + board.vex ----------------
#
# gen_vlog is a python script. platforms/AgRV/builder/main.py has two
# shapes; the one that leaves a Quartus-ready directory ("prelogic") is
#
#   python3 gen_vlog -s -c board.hx -d DEVICE -x board.vex board.ve board.v
#
# - `-s` makes gen_vlog emit the netlist sub-flavour the SDK's
#   pre_logic.tcl consumes, and the file is board.v, not board.vx: board.v
#   is what carries the pin bindings (assign PIN_43_in = GPIO1_0; ...).
#   Without -s / with a .vx output the directory had no board.v at all and
#   Quartus had nothing to compile (reported from the dev board).
# The alternative shape in main.py (no -s, output board.vx) is for the
# "direct" flow where gen_logic.tcl does the same expansion later.
#
# We still keep board.vx around for tools that read the pre-expansion form;
# tools/check_pinctrl.py --netlist wants board.v's flavour.

# --- Your own logic (the IP the pin map already describes) -------------
#
# AGM_USER_RTL is a space-separated list of Verilog files that make up an IP
# this board's pin map refers to -- e.g. the full-duplex SPI wrapper whose
# ports are exactly the `csn` / `sck` / `si_io0` / `so_io1` / `*_out_en` nets
# gen_vlog emits for a VE that has the case-C + case-B rows (the vendor's own
# `ip_name` flow does the same thing through pre_logic.tcl's IP_VV). Each file
# is copied next to board.v (so the project stays self-contained and travels to
# a Quartus workstation as one directory) and handed to pre_logic.tcl as
# IP_VV, which appends it to the project's VERILOG_FILES /
# `set_global_assignment -name VERILOG_FILE` list.
#
# This stages the sources; Step 0b below is what gets the module *instantiated*
# (gen_vlog's `-m <ip>.v`, the vendor's own flag).
USER_RTL_STAGED=""
for _v in ${AGM_USER_RTL:-}; do
	[ -f "$_v" ] || {
		echo "Error: AGM_USER_RTL file not found: $_v" >&2
		exit 4
	}
	cp -f "$_v" "$LOGIC_DIR/"
	USER_RTL_STAGED="$USER_RTL_STAGED $(basename "$_v")"
done
USER_RTL_STAGED=${USER_RTL_STAGED# }
if [ -n "$USER_RTL_STAGED" ]; then
	echo ">>> [2/2] user RTL staged into $LOGIC_DIR: $USER_RTL_STAGED"
fi

# --- Step 0b: the declared IP, before board.v is generated -------------
#
# The vendor's own flow prepares a logic IP as its own project first
# (platforms/AgRV/builder/main.py: ipvlog + a pre_logic run with
# logic_ip/IP_DESIGN/IP_INSTALL_DIR, whose post-script installs
# <ip>_.ve/.sdc and the netlist), and the *board* run then consumes that
# ve with `gen_vlog ... -m <ip.v> -i <ip>.ve` -- the -m/-i pair in the
# builder's normal VLOGERCMD. The IP's ve is what makes gen_vlog emit
# the IP's nets and its instantiation instead of the "no slave" default
# assigns (the ones Quartus then flags as 12014/12015 once an IP drives
# them).
#
# The instantiation itself comes from Step 1's `-m <ip>.v`: gen_vlog reads
# the module header and emits `<module> macro_inst(.port (port), ...)`, the
# port-name == net-name convention the vendor's generated design uses. (An
# earlier revision injected that instance with a script of our own,
# tools/ip_instance.py; the -m flag replaced it, and the script is gone.)
IP_VE=""
IP_NAME=${AGM_LOGIC_IP:-$(basename "${USER_RTL_STAGED%% *}" .v)}
if [ -n "$USER_RTL_STAGED" ] && [ "${AGM_LOGIC_IP_FLOW:-on}" != "off" ]; then
	LOGIC_IP_DIR=${AGM_LOGIC_IP_DIR:-$(dirname "$LOGIC_DIR")/logic_ip}
	mkdir -p "$LOGIC_IP_DIR"
	for _v in $USER_RTL_STAGED; do cp -f "$LOGIC_DIR/$_v" "$LOGIC_IP_DIR/"; done
	echo ">>> [0b/2] IP prepare: $IP_NAME (logic_ip = $LOGIC_IP_DIR)"
	# The vendor's IP step starts with `gen_vlog -c <ip>.hx -s -m <ip>.v
	# -i <ip>_.ve <board>.ve` (builder/main.py's IPVLOGERCMD): it emits the
	# IP's header and *writes* the IP's ve next to it (`-i` names the file
	# gen_vlog fills in -- "Creating/Overwriting IP VE file"). Without the
	# .hx the pre_logic run below stops with `couldn't open <ip>.hx`
	# .
	( cd "$LOGIC_IP_DIR" && "$PYTHON" "$GEN_VLOG" \
		-c "$IP_NAME.hx" -s -m "$IP_NAME.v" -d "$LOGIC_DEVICE" \
		-i "${IP_NAME}_.ve" "$LOGIC_DIR/board.ve" ) 2>&1 | tail -3
	( cd "$LOGIC_IP_DIR" && "$AF_CMD" -L ip_pre_logic.log \
		-X "set logic_ip   {true}" \
		-X "set LOGIC_DEVICE {$LOGIC_DEVICE}" \
		-X "set LOGIC_DESIGN {$IP_NAME}" \
		-X "set LOGIC_MODULE {$IP_NAME}" \
		-X "set IP_DESIGN  {$IP_NAME}" \
		-X "set IP_INSTALL_DIR {.}" \
		-X "set LOGIC_DIR {.}" \
		-X "set LOGIC_VV  {$IP_NAME.v}" \
		-X "set BOARD_ASF {}" -X "set BOARD_PRE {}" -X "set BOARD_POST {}" \
		-X "set LOGIC_FORCE false" -X "set VEX_FILE {}" -X "set ATF_FILE {}" \
		-X "set SDC_FILE {}" -X "set ORIGINAL_PIN {-}" \
		-F "$PRE_LOGIC" ) || true
	for _cand in "$LOGIC_IP_DIR/$IP_NAME.ve" "$LOGIC_IP_DIR/${IP_NAME}_.ve"; do
		[ -f "$_cand" ] && IP_VE="$_cand" && break
	done
	if [ -n "$IP_VE" ]; then
		echo ">>> [0b/2] gen_vlog will use -m $IP_NAME.v -i $IP_VE"
	else
		echo ">>> [0b/2] note: no $IP_NAME.ve produced by the IP run -- board.v will"
		echo ">>>        still carry the IP's instance (Step 1's -m), but nothing"
		echo ">>>        about it was prepared in $LOGIC_IP_DIR."
	fi
fi

# The pre-expansion netlist, for anything that still reads it (it has the
# same pin bindings, only without the -s expansion).
( cd "$LOGIC_DIR" && "$PYTHON" "$GEN_VLOG" \
	-c board.hx \
	-d "$LOGIC_DEVICE" \
	"$LOGIC_DIR/board.ve" \
	board.vx \
	-x board.vex ) >/dev/null 2>&1 || true

# `-m` whenever the pin map has a user IP: it is what makes gen_vlog emit the
# instance. The paths have no spaces, so the deliberate word splitting of
# $GEN_VLOG_IP is safe.
GEN_VLOG_IP=""
if [ -n "$USER_RTL_STAGED" ]; then
	# main.py's -s board run carries `-m <ip.v>` and no -i (the -i belongs
	# to the non -s netlist run, whose -m is the IP's own compiled .vx).
	GEN_VLOG_IP="-m $IP_NAME.v"
fi

echo ">>> [1/2] gen_vlog: $LOGIC_DIR/board.ve → $LOGIC_DIR/"
( cd "$LOGIC_DIR" && "$PYTHON" "$GEN_VLOG" \
	-c board.hx \
	-s \
	-d "$LOGIC_DEVICE" \
	"$LOGIC_DIR/board.ve" \
	board.v \
	-x board.vex $GEN_VLOG_IP )

# --- Step 1c: the pin constraints must name ports board.v has ---------
#
# gen_vlog names a pin after whichever row claims it, and the two flavours
# disagree: with the IP (`-m`, the run above) the case-B signals win, so
# board.vex says `si_io0 PIN_92`; without it the MCU function wins and the
# same pin becomes `SPI0_SI_IO0 PIN_92`. Quartus ignores a location
# assignment whose target does not exist and quietly auto-places that port
# (: a board.v from the -m run shipped next to a.vex from
# the other flavour left the flash's CS/SCK/MOSI/MISO unconstrained -- RDID
# came back 00 00 00 and neither the flow nor Quartus said anything).
# So: every `NAME PIN_<n>` row of board.vex must be a top-level port of
# board.v. AGM_ALLOW_VEX_PORT_MISMATCH=1 downgrades it to a warning for a
# pin map that deliberately relies on one of gen_vlog's name rewrites.
_agm_vex_check() {
	"$PYTHON" - "$LOGIC_DIR/$LOGIC_DESIGN.v" "$LOGIC_DIR/$LOGIC_DESIGN.vex" \
		<<'PY'
import re
import sys

netlist, vex = sys.argv[1], sys.argv[2]
try:
    with open(netlist) as f:
        text = f.read()
except OSError as e:
    print(f"cannot read {netlist}: {e}", file=sys.stderr)
    sys.exit(1)

ports = set(re.findall(
    r"(?m)^\s*(?:inout|input|output)\s+([A-Za-z_][\w$]*)\s*;", text))
missing = []
with open(vex) as f:
    for line in f:
        m = re.match(r"\s*([A-Za-z_][\w$]*)\s+PIN_([A-Za-z0-9_]+)\s*$", line)
        if m and m.group(1) not in ports:
            missing.append((m.group(1), m.group(2)))

if missing:
    print(f"Error: {vex} constrains pin(s) that {netlist} has no port for:",
          file=sys.stderr)
    for name, pin in missing:
        print(f"         {name} -> PIN_{pin}", file=sys.stderr)
    print("       Quartus would ignore those and auto-place the port, so the "
          "bitstream\n       would leave that pin unconnected. Regenerate "
          "board.v and board.vex in one\n       gen_vlog run (this script "
          "does that; the two files travel together), or\n       set "
          "AGM_ALLOW_VEX_PORT_MISMATCH=1 if the mismatch is intended.",
          file=sys.stderr)
    sys.exit(1)
PY
}
if ! _agm_vex_check; then
	if [ -n "${AGM_ALLOW_VEX_PORT_MISMATCH:-}" ]; then
		echo ">>> [1/2] warning: keeping a pin map whose names do not match " \
			"board.v (AGM_ALLOW_VEX_PORT_MISMATCH)" >&2
	else
		exit 8
	fi
fi
echo ">>> [1/2] pin constraints name $(grep -c 'PIN_' "$LOGIC_DIR/$LOGIC_DESIGN.vex") pin(s) board.v has"

# --- Step 2: pre_logic.tcl → board.asf + board.qsf + af_quartus.tcl ----
#
# Supra pre_logic.tcl emits the Quartus-ready project files (af_quartus.tcl
# copies the Quartus wrapper + writes board.qsf + creates board.asf the
# Supra post-Quartus step needs).
#
# After this step the user takes $LOGIC_DIR to a Quartus workstation
# and runs:
#   quartus_sh -t af_quartus.tcl
# then runs Supra gen_logic.tcl to compile the .vqm into example_board.bin.

# --- ASF inputs --------------------------------------------------------
#
# pre_logic.tcl folds two files into <logic_dir>/board.asf: BOARD_ASF and
# DESIGN_ASF go *inside* its "# pio_begin/# pio_end" block, and whatever
# was already written outside that block is preserved verbatim (read_file
# strips the block, so nothing accumulates or duplicates across runs).
#
#   BOARD_ASF  a hand-written <board_dir>/board.asf, when the board has one
#              (vendor layout: the board owns this file). Escape hatch for
#              assignments the dts does not model yet (drive strength,
#              CFG_KEEP, WKUP config, …).
#   DESIGN_ASF the file generate_board_ve.py renders from the pin map
#              (agm,pull-ups / agm,pull-downs today).
BOARD_ASF_FILE=""
[ -f "$BOARD_DIR/board.asf" ] && BOARD_ASF_FILE="$BOARD_DIR/board.asf"
DESIGN_ASF_FILE=""
[ -f "$LOGIC_DIR/board.generated.asf" ] && \
	DESIGN_ASF_FILE="$LOGIC_DIR/board.generated.asf"

if [ -n "$DESIGN_ASF_FILE" ]; then
	echo ">>> [2/2] pre_logic.tcl (pulls from $DESIGN_ASF_FILE)"
else
	echo ">>> [2/2] pre_logic.tcl"
fi
( cd "$LOGIC_DIR" && "$AF_CMD" \
	-L pre_logic.log \
	-X "set LOGIC_DEVICE {$LOGIC_DEVICE}" \
	-X "set LOGIC_DESIGN {$LOGIC_DESIGN}" \
	-X "set LOGIC_MODULE {$LOGIC_MODULE}" \
	-X "set LOGIC_TOPPIN {$LOGIC_TOPPIN}" \
	-X "set LOGIC_DIR   {.}" \
	-X "set LOGIC_VV    {board.v}" \
	-X "set BOARD_ASF   {$BOARD_ASF_FILE}" \
	-X "set BOARD_PRE   {}" \
	-X "set BOARD_POST  {}" \
	-X "set DESIGN_ASF  {$DESIGN_ASF_FILE}" \
	-X "set DESIGN_PRE  {}" \
	-X "set DESIGN_POST {}" \
	-X "set LOGIC_FORCE false" \
	-X "set VEX_FILE    {board.vex}" \
	-X "set ATF_FILE    {}" \
	-X "set SDC_FILE    {}" \
	-X "set IP_VV       {$USER_RTL_STAGED}" \
	-X "set LIB_DIRS    {}" \
	-X "set ORIGINAL_PIN {-}" \
	-F "$PRE_LOGIC" )

# --- Step 2b: make the Quartus project self-contained ------------------
#
# pre_logic.tcl references the vendor architecture netlist (alta_sim.v:
# alta_rio / alta_pllve / alta_gclksw / alta_gclkgen / alta_io_gclk /
# alta_rv32) by its SDK package path. The vendor's own prepare-logic run on a
# Windows workstation bakes in
#   C:\Users\...\.platformio\packages\tool-agrv_logic\etc\arch\rodinia\...
# -- generated here it is $HOME/..., which the workstation cannot read,
# and Quartus then reports "undefined entity alta_rio" for every instance
# (reported from the dev board). Ship the netlists next to board.v and
# reference them relatively instead. AGM_ALTA_DIR overrides the source dir.
ALTA_DIR=${AGM_ALTA_DIR:-$HOME/AgRV_pio/packages/tool-agrv_logic/etc/arch/rodinia}
QSF="$LOGIC_DIR/$LOGIC_DESIGN.qsf"
if [ -f "$ALTA_DIR/alta_sim.v" ] && [ -f "$QSF" ]; then
	cp -f "$ALTA_DIR"/alta_*.v "$LOGIC_DIR/" 2>/dev/null || true
	sed -i -E 's#^set_global_assignment -name VERILOG_FILE ".*/(alta_[A-Za-z0-9_]+)\.v"?$#set_global_assignment -name VERILOG_FILE "\1.v"#' "$QSF"
	# pre_logic.tcl hard-codes the .vx name even when LOGIC_VV is the .v
	# flavour that Quartus needs (same as it hard-codes ${DESIGN}.hx).
	sed -i -E "s#^(set_global_assignment -name VERILOG_FILE \")$LOGIC_DESIGN\.vx\"#\1$LOGIC_DESIGN.v\"#" "$QSF"
	echo ">>> [2/2] vendor netlist staged: alta_*.v → $LOGIC_DIR/"
else
	echo ">>> [2/2] warning: $ALTA_DIR/alta_sim.v not found (set AGM_ALTA_DIR)" >&2
fi

# --- Step 3: stage agrv_fpga_decomp.inc ------------------------------
# SDK cfg's `check_logic` proc reads this file. Static SDK asset;
# copy into LOGIC_DIR so any flash invocation that runs check_logic
# can find it via the script_path argument.
cp "$AGRV_PLATFORM_ETC/agrv_fpga_decomp.inc" "$LOGIC_DIR/agrv_fpga_decomp.inc"

echo ""
echo ">>> Quartus-ready logic directory ready at: $LOGIC_DIR"
echo "    Next step (Quartus side, wherever it is installed):"
echo "      cd $LOGIC_DIR"
echo "      cd $LOGIC_DIR && quartus_sh -t af_quartus.tcl"
echo "    Then let Supra do the placement+routing and write the bitstream"
echo "    (this runs anywhere, the Linux Supra package is enough):"
echo "      $SCRIPT_DIR/compile_bitstream.sh $LOGIC_DIR"
echo "    In a Zephyr build this is just: west build -t bitstream"
echo "    (it writes <build_dir>/zephyr/board.bin, where west flash looks)."
