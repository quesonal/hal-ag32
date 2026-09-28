#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Compile the FPGA bitstream from a Quartus-routed logic directory.
#
# This is the last step of the bitstream flow:
#
#   dtsi ──(build_bitstream.sh)──> logic/ ──(Quartus, user's side)──> .vo
#                                                                     │
#                                     this script ──(Supra af_cmd)────┘
#                                                     │
#                                                     ▼
#                                                board.bin
#
# Supra does its own placement + routing on the architecture database and
# writes the configuration image; the Verilog it consumes is Quartus's
# post-route netlist (simulation/modelsim/<design>.vo), so <logic_dir> must
# have been through `quartus_sh -t af_quartus.tcl` first.
#
# Usage:
#   compile_bitstream.sh <logic_dir> [<bitstream.bin>]
#
#   <logic_dir>      directory produced by build_bitstream.sh
#   <bitstream.bin>  where to copy the result (default: leave it in place;
#                    AGM_BITSTREAM_BIN is used when set and no argument is
#                    given, which is the file `west flash` looks for)
#
# Environment:
#   AGRV_SDK_PATH   root of the AgRV PlatformIO install (default ~/AgRV_pio)
#   SUPRA_HOME      tool-agrv_logic package (default $AGRV_SDK_PATH/packages/tool-agrv_logic)
#   AGM_LOGIC_DESIGN  design basename (auto-detected: the single
#                     <logic_dir>/*.bin that is not *_batch.bin -- the Zephyr
#                     flow names it `board`, per Quartus TOP_LEVEL_ENTITY)
#   AGM_SUPRA_X     extra "-X" options, already quoted as one argument
#   AGM_SUPRA_QUARTUS_SDC / _FITTING / _FITTER / _EFFORT / _HOLDX / _SKEW
#                   override the individual settings below (they are the
#                   values the vendor's Windows flow uses)
#
#: this reproduces the Windows Supra result (same tool
# version 2026.03.b0, same "0 fatals, 0 errors, 1 warnings, 88 infos", same
# 99944-byte image) and the image it produces boots the board. It is *not*
# byte-reproducible -- the placer/routing is seeded per run, so two runs (or
# a run against the Windows artifact) differ by a few hundred bytes spread
# over the image. Judge the result by "it boots", not by md5.

set -eu

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
	echo "Usage: $0 <logic_dir> [<bitstream.bin>]" >&2
	exit 1
fi

LOGIC_DIR=$1
if [ ! -d "$LOGIC_DIR" ]; then
	echo "Error: no such logic directory: $LOGIC_DIR" >&2
	exit 2
fi
LOGIC_DIR=$(cd "$LOGIC_DIR" && pwd)

AGRV_SDK_PATH=${AGRV_SDK_PATH:-$HOME/AgRV_pio}
SUPRA_HOME=${SUPRA_HOME:-$AGRV_SDK_PATH/packages/tool-agrv_logic}
AF_CMD=$SUPRA_HOME/bin/af_cmd

if [ ! -x "$AF_CMD" ]; then
	echo "Error: Supra not found at $AF_CMD" >&2
	echo "Set SUPRA_HOME to the tool-agrv_logic package directory." >&2
	exit 3
fi

# Which design are we compiling? build_bitstream.sh names everything after
# AGM_LOGIC_DESIGN (board), older hand-made directories after the project.
if [ -n "${AGM_LOGIC_DESIGN:-}" ]; then
	DESIGN=$AGM_LOGIC_DESIGN
else
	DESIGN=$(basename "$(ls "$LOGIC_DIR"/*.bin 2>/dev/null | grep -v _batch.bin | head -1)" .bin)
	if [ -z "$DESIGN" ]; then
		DESIGN=$(basename "$(ls "$LOGIC_DIR"/*.asf 2>/dev/null | head -1)" .asf)
	fi
	if [ -z "$DESIGN" ]; then
		echo "Error: cannot tell the design name; set AGM_LOGIC_DESIGN" >&2
		exit 4
	fi
fi

VO=$LOGIC_DIR/simulation/modelsim/$DESIGN.vo
if [ ! -f "$VO" ]; then
	echo "Error: $VO not found." >&2
	echo "That is Quartus's post-route netlist: run" >&2
	echo "  quartus_sh -t af_quartus.tcl" >&2
	echo "in $LOGIC_DIR on a Quartus installation first (Windows or Linux)." >&2
	exit 5
fi

QUARTUS_SDC=${AGM_SUPRA_QUARTUS_SDC:-true}
FITTING=${AGM_SUPRA_FITTING:-Auto}
FITTER=${AGM_SUPRA_FITTER:-full}
EFFORT=${AGM_SUPRA_EFFORT:-high}
HOLDX=${AGM_SUPRA_HOLDX:-default}
SKEW=${AGM_SUPRA_SKEW:-basic}

echo ">>> Supra compile: $DESIGN in $LOGIC_DIR"
echo "    vo: $VO"
( cd "$LOGIC_DIR" && "$AF_CMD" -B --batch --mode QUARTUS \
	-X "set QUARTUS_SDC $QUARTUS_SDC" \
	-X "set FITTING $FITTING" \
	-X "set FITTER $FITTER" \
	-X "set EFFORT $EFFORT" \
	-X "set HOLDX $HOLDX" \
	-X "set SKEW $SKEW" \
	${AGM_SUPRA_X:-} )

BIN=$LOGIC_DIR/$DESIGN.bin
if [ ! -f "$BIN" ]; then
	echo "Error: Supra finished without producing $BIN" >&2
	exit 6
fi

echo ">>> bitstream: $BIN ($(stat -c %s "$BIN") bytes, md5 $(md5sum "$BIN" | cut -d' ' -f1))"
echo "    batch file: $LOGIC_DIR/${DESIGN}_batch.bin"

# An uncompressed Supra config is exactly 99944 bytes (FCB_AUTO_WORDS = 24986
# words). A different size means the project asked Supra to compress it
# (`logic_compress = true` / `set LOGIC_COMPRESS true`), which is a *different*
# layout on the board: the flashing tool puts its decompression algorithm in
# front of the config, and the device has to be built with
# CONFIG_AGM_FCB_BITSTREAM_COMPRESSED to stream it. Flashing one of these into
# a board that streams raw words leaves the fabric dead (recovery: BOOT0).
BIN_SIZE=$(wc -c < "$BIN" | tr -d ' ')
if [ "$BIN_SIZE" != 99944 ]; then
	echo "WARNING: $BIN is $BIN_SIZE bytes, not the uncompressed 99944." >&2
	echo "         That is the compressed form: build the loader with" >&2
	echo "         CONFIG_AGM_FCB_BITSTREAM_COMPRESSED=y (and see the address" >&2
	echo "         shift the fabric bitstream), or make an" >&2
	echo "         uncompressed bitstream. tools/agm_oo.sh bitstream and" >&2
	echo "         tools/flash_logic.sh refuse this size unless" >&2
	echo "         AGM_BITSTREAM_ANY_SIZE=1 is set." >&2
fi

DEST=${2:-${AGM_BITSTREAM_BIN:-}}
if [ -n "$DEST" ] && [ "$DEST" != "$BIN" ]; then
	mkdir -p "$(dirname "$DEST")"
	cp "$BIN" "$DEST"
	echo ">>> copied to $DEST (west flash looks here)"
fi
