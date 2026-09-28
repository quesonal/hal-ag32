#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# DEPRECATED (Plan A): `west flash` is the unified
# entry point now. Use:
#   west flash                       # write both (agrv_openocd is the default)
#   west flash --skip-bitstream      # flash_fw.sh's job
#   west flash --bitstream-only      # this script's job
# (all three belong to the default agrv_openocd runner; the ROM-bootloader
#  runner --runner agrv32flash has no --bitstream-only)
# Direct invocation of this file keeps running so muscle-memory
# CI hooks don't break, but the `west build -t flash` /
# `-t flash-logic` CMake targets are gone -- call west flash
# with the flag above. Slated for removal once no external
# scripts reference this entry point.
#
# Flash the FPGA bitstream to FLASH @ ${AGM_BITSTREAM_ADDR} (default
# 0x800e7000) using the SDK's `agrv write_fpga_config` proc. This does
# its own sector-erase of just the bitstream region, so a previously
# flashed firmware in the rest of the bank survives. Then `reset run`
# so the FPGA loads the new bitstream.
#
# This is the surgical counterpart to `west flash --runner agrv_openocd`
# (which does `flash write_image erase` for the bitstream too, erasing
# the firmware in the same bank). Use this during bitstream-iteration
# development — you do NOT need to rebuild the firmware after a
# bitstream change, and you do NOT need Quartus+Supra to re-run if
# the bitstream is already at ${AGM_BITSTREAM_BIN}.
#
# Usage:
#   flash_logic.sh [<bitstream.bin>]
#
# Bitstream path resolution (first match wins):
#   1. CLI arg
#   2. $AGM_BITSTREAM_BIN env
#   3. <build>/logic/board.bin       (Supra output of west build -t bitstream;
#      the board-dir fallback below is previously)
#
# Environment overrides:
#   AGM_BITSTREAM_ADDR   bitstream FLASH address     default: 0x800e7000
#   AGRV_ADAPTER         cmsis-dap | jlink           default: cmsis-dap
#   AGRV_PLATFORM_ETC    platforms/AgRV/etc path     default: ~/AgRV_pio/platforms/AgRV/etc
#   AGRV_OPENOCD         openocd binary path         default: ~/AgRV_pio/packages/tool-agrv_openocd/bin/openocd_cmd
#   ZEPHYR_HAL_AGM_HOME  hal_ag32 checkout path       default: the tree this script lives in
#   ZEPHYR_HAL_AGM_TOOLS tools subdir                default: $ZEPHYR_HAL_AGM_HOME/tools
#   AGM_OPENOCD_ATTEMPTS whole-session retries       default: 4
#
# After this script, the OPTBY FPGA CONFIG pointer is auto-set to the
# bitstream address by `agrv write_fpga_config`, so a subsequent CPU
# reset will load the new bitstream.

set -eu

# --- venv guard  ----
# See tools/flash_fw.sh for the rationale.
if [ -z "${VIRTUAL_ENV:-}" ]; then
	echo "FATAL: no Zephyr virtualenv active - activate the repo venv first" >&2
	exit 3
fi
PYTHON=${PYTHON:-$VIRTUAL_ENV/bin/python3}

BS_BIN=${1:-${AGM_BITSTREAM_BIN:-${BOARD_DIR}/logic/board.bin}}
BS_ADDR=${AGM_BITSTREAM_ADDR:-0x800e7000}

if [ ! -f "$BS_BIN" ]; then
	echo "Error: bitstream not found at $BS_BIN" >&2
	echo "Set \$AGM_BITSTREAM_BIN, pass as arg, or build the Quartus" >&2
	echo "directory via 'west build -t logic', compile on a Quartus" >&2
	echo "workstation, then run 'west build -t bitstream' (writes" >&2
	echo "board.bin into the build dir)." >&2
	exit 2
fi

# Same guard as the west runner and tools/agm_oo.sh: an uncompressed Supra
# config is exactly 99944 B, and the Zephyr FCB driver streams raw words unless
# the image was built with CONFIG_AGM_FCB_BITSTREAM_COMPRESSED. Anything else is
# the SDK's compressed/encrypted form, and writing one for a build that cannot
# stream it leaves the board with a dead fabric (recovery: BOOT0).
BS_SIZE=$(wc -c < "$BS_BIN" | tr -d ' ')
if [ "$BS_SIZE" != 99944 ] && [ "${AGM_BITSTREAM_ANY_SIZE:-0}" != 1 ]; then
	echo "Error: $BS_BIN is $BS_SIZE B; an uncompressed Supra bitstream is 99944 B." >&2
	echo "A compressed image needs CONFIG_AGM_FCB_BITSTREAM_COMPRESSED=y on the" >&2
	echo "device plus the config-address shift -- see" >&2
	echo "the fabric bitstream. Set AGM_BITSTREAM_ANY_SIZE=1 to" >&2
	echo "write it anyway." >&2
	exit 4
fi

# --- Locate tools --------------------------------------------------
# This script lives in <module>/tools/, so the module root is its parent.
SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ZEPHYR_HAL_AGM_HOME=${ZEPHYR_HAL_AGM_HOME:-$(dirname -- "$SELF_DIR")}
ZEPHYR_HAL_AGM_TOOLS=${ZEPHYR_HAL_AGM_TOOLS:-$ZEPHYR_HAL_AGM_HOME/tools}
AGRV_SDK_PATH=${AGRV_SDK_PATH:-$HOME/AgRV_pio}
AGRV_PLATFORM_ETC=${AGRV_PLATFORM_ETC:-$AGRV_SDK_PATH/platforms/AgRV/etc}
AGRV_OPENOCD=${AGRV_OPENOCD:-$AGRV_SDK_PATH/packages/tool-agrv_openocd/bin/openocd_cmd}
AGRV_ADAPTER=${AGRV_ADAPTER:-cmsis-dap}

# Probe setup comes from the board's support/openocd.cfg, not from the SDK's
# agrv2k.cfg directly -- see the same block in flash_fw.sh: only the board cfg
# forces the CMSIS-DAP v2 (bulk) backend and the vendor's 10 MHz SWD clock,
# and without them AP accesses fail on this board ("stalled AP operation",
# dmstatus=0x0).  Prefer the workspace copy, which every build re-renders.
BOARD=${BOARD:-agrv2k_407}
SUPPORT_CFG=""
for _cand in "${AGM_WORKSPACE:-$HOME/zephyrproject}/modules/hal_ag32" \
	     "$ZEPHYR_HAL_AGM_HOME"; do
	if [ -f "$_cand/zephyr/module.yml" ]; then
		SUPPORT_CFG="${AGM_OPENOCD_CFG:-}"
		[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG="${AGM_BUILD_DIR:+${AGM_BUILD_DIR}/logic/openocd.cfg}"
		[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG=$(ls -t "${AGM_WORKSPACE:-$HOME/zephyrproject}"/build*/logic/openocd.cfg /tmp/b_*/logic/openocd.cfg 2>/dev/null | head -1)
		[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG="$_cand/boards/agm/$BOARD/support/openocd.cfg"
		break
	fi
done
if [ -z "$SUPPORT_CFG" ]; then
	echo "Error: board support cfg not found for $BOARD" >&2
	echo "Run a west build once so the module renders support/openocd.cfg." >&2
	exit 3
fi

if [ ! -x "$AGRV_OPENOCD" ]; then
	echo "Error: openocd not found at $AGRV_OPENOCD" >&2
	echo "Set AGRV_OPENOCD or install tool-agrv_openocd." >&2
	exit 3
fi
if [ ! -x "$ZEPHYR_HAL_AGM_TOOLS/openocd_warmup.py" ]; then
	echo "Error: openocd_warmup.py not found at $ZEPHYR_HAL_AGM_TOOLS/openocd_warmup.py" >&2
	echo "Set ZEPHYR_HAL_AGM_HOME." >&2
	exit 3
fi

# --- Warmup probe + flash bitstream + reset -----------------------
# Sequence:
#   1. warmup:    slow pyusb GET_DESCRIPTOR for the CMSIS-DAP probe
#                 (FW 2.1.0 string-read timeout workaround)
#   2. init:      AgRV SDK cfg sources interface/cmsis-dap.cfg,
#                 switches to usb_bulk backend, examines RISC-V core
#   3. halt:      stop CPU so flash writes don't race
#   4. write:     agrv write_fpga_config — SDK's purpose-built proc
#                 that erases just the bitstream sectors and writes
#                 the bitstream, then auto-sets OPTBY FPGA CONFIG
#                 pointer to $BS_ADDR
#   5. reset run: FPGA loads the new bitstream (via OPTBY pointer),
#                 CPU boots whatever firmware is in FLASH (or stalls
#                 on illegal instruction if no firmware is present)
#   6. shutdown:  close openocd session, release probe

echo ">>> bitstream: $BS_BIN"
echo ">>> addr:      $BS_ADDR"
echo ">>> [1/6] warmup probe (AGM CMSIS-DAP FW 2.1.0 string-read workaround)"
"$PYTHON" "$ZEPHYR_HAL_AGM_TOOLS/openocd_warmup.py"

echo ">>> [2/6] openocd init (adapter=$AGRV_ADAPTER)"
echo ">>> [3/6] halt"
echo ">>> [4/6] agrv write_fpga_config (auto-erase bitstream sector + set OPTBY FPGA CONFIG)"
echo ">>> [5/6] reset run (FPGA loads new bitstream)"
echo ">>> [6/6] shutdown"

# The bitstream path is parsed by openocd's TCL interpreter, which splits an
# unquoted argument on whitespace ("/tmp/logic - 100mhz/example_board.bin"
# becomes two words, and the SDK proc then dies with
# "Unknown image type: 100mhz/example_board.bin"). Brace-quote it into a
# single TCL word -- the SDK's own `oo` runner emits the same form.
#
# ADAPTER_SPEED has to stay 10 MHz: the board cfg defaults to it and AGM_SWD_SPEED
# overrides it for margin experiments. (Sourcing the SDK's agrv2k.cfg directly
# used to give 15 MHz -> 14285 kHz plus the v1 HID backend, and AP accesses came
# back as "stalled AP operation"; do not go back to that.)
# The examine fails intermittently on this dev board (2 runs in 6,
# with and without the SDK cfg), so retry the whole session before failing.
ATTEMPTS=${AGM_OPENOCD_ATTEMPTS:-4}
attempt=1
while :; do
	OCD_RC=0
	OCD_OUT=$("$AGRV_OPENOCD" \
		-s "$AGRV_PLATFORM_ETC" \
		-c "variable ADAPTER $AGRV_ADAPTER" \
		-c "variable ADAPTER_SPEED ${AGM_SWD_SPEED:-10000}" \
		-f "$SUPPORT_CFG" \
		-c "init" \
		-c "reset init" \
		-c "agrv write_fpga_config 0 {$BS_BIN} $BS_ADDR" \
		-c "reset run" \
		-c "shutdown" 2>&1) || OCD_RC=$?
	printf '%s\n' "$OCD_OUT" | tail -8

	case "$OCD_OUT" in
	# TODO: also retry on "Error: checksum mismatch - attempting binary compare"
	# (openocd verify_image false positive under SWD flake;
	# observed on this dev board, bitstream actually fine -- FCB STAT 0x000f0002
	# on the next reset, second flash_logic.sh invocation produces 0 byte diff).
	*"Examination failed"* | *"stalled AP"* | *"dmstatus=0x0"* | \
	*"Couldn't determine state"* | *"examine-end failed"* | \
	*"Flash write data timed out"* | *"error writing to flash"* | \
	*"failed to write FPGA"*)
		if [ "$attempt" -lt "$ATTEMPTS" ]; then
			echo "WARN: openocd session failed (attempt $attempt/$ATTEMPTS) - retrying" >&2
			attempt=$((attempt + 1))
			sleep 1
			continue
		fi ;;
	esac
	break
done

if printf '%s' "$OCD_OUT" | grep -q "agrv wrote fpga configuration"; then
	exit 0
fi
echo "FATAL: bitstream write failed (rc=$OCD_RC)" >&2
exit 1
