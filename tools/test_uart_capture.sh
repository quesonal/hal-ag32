#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# test_uart_capture.sh — capture the AgRV2K UART0 console on /dev/ttyACM0
#                        around a reset, and (optionally) flash firmware first.
#
# Chain under test
#   printk → uart_agm (native PL011-register-compatible driver) →
#     AFSEL (GPIO6_1=RX / GPIO7_6=TX) → PIN_68/69 → USB-UART bridge inside
#     the FPGA bitstream → USB CDC-ACM → host /dev/ttyACM0
#     (115200 8N1, probe's own serial bridge)
#
# The capture reader is started BEFORE the reset, so the one-shot boot
# banner is not missed. (Opening the port after `reset run` loses it: the
# banner is printed within the first milliseconds of boot.)
#
# Usage
#   ./test_uart_capture.sh [options] [firmware.bin|firmware.elf]
#
#   With a firmware argument the script does a full cycle:
#       warmup probe → erase firmware region → write → verify → reset run
#   Without one it just does `reset run` (whatever is already in FLASH).
#
# Options
#   -b <board>     board name                (default agrv2k_407)
#   -t <seconds>   capture window            (default 10)
#   -o <path>      capture file              (default /tmp/uart_capture_<board>_<ts>.bin)
#   -d <device>    serial device             (default /dev/ttyACM0)
#   -n             no erase (write without erasing the firmware region)
#
# Exit codes
#   0  banner / expected text captured, printable ratio healthy
#   1  capture empty (probe bridge down, or CPU never ran)
#   2  capture looks like garbage (baud / framing mismatch)
#   3  preflight failure (tool, cfg, or flash step failed)
#
# Environment
#   AGM_OPENOCD_ATTEMPTS  whole-session retries for the flash step (default 3)
#
# Prerequisites
#   * an active Zephyr virtualenv (repo venv; do NOT use system python)
#   * AgRV SDK openocd + platform cfg (~/AgRV_pio)
#   * probe plugged in (cmsis-dap), cdc_acm bound to /dev/ttyACM0
#
# The firmware region stops at the bitstream: FLASH is 1 MB @0x80000000 and
# the last 100 KB (0x800E7000..0x800FFFFF) holds the FPGA bitstream, which
# this script never erases.

set -u
set -o pipefail

# ---------- knobs -----------------------------------------------------------
BOARD="agrv2k_407"
TIMEOUT=10
TTY_DEV="/dev/ttyACM0"
DO_ERASE=1
FW_FILE=""

while getopts "b:t:o:d:nh" opt; do
	case "$opt" in
	b) BOARD="$OPTARG" ;;
	t) TIMEOUT="$OPTARG" ;;
	o) OUTFILE="$OPTARG" ;;
	d) TTY_DEV="$OPTARG" ;;
	n) DO_ERASE=0 ;;
	h) sed -n '2,40p' "$0"; exit 0 ;;
	*) echo "bad option; try -h" >&2; exit 3 ;;
	esac
done
shift $((OPTIND - 1))
[ $# -ge 1 ] && FW_FILE="$1"

OUTFILE="${OUTFILE:-/tmp/uart_capture_${BOARD}_$(date +%Y%m%d_%H%M%S).bin}"

FW_LOAD_ADDR="0x80000000"   # ROM bootloader reads the raw BIN from FLASH base
BS_ADDR="0x800e7000"        # bitstream start (last 100 KB of FLASH)
FW_ERASE_LEN="0xe7000"      # erase exactly up to the bitstream, never beyond

AGRV_SDK_PATH="${AGRV_SDK_PATH:-$HOME/AgRV_pio}"
AGRV_ADAPTER="${AGRV_ADAPTER:-cmsis-dap}"
OPENOCD_CMD="$AGRV_SDK_PATH/packages/tool-agrv_openocd/bin/openocd_cmd"
PLATFORM_ETC="${AGRV_PLATFORM_ETC:-$AGRV_SDK_PATH/platforms/AgRV/etc}"

# ---------- locate hal_ag32 tree (support cfg + warmup + python) -------------
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
AGM_HOME="${ZEPHYR_HAL_AGM_HOME:-}"
if [ -z "$AGM_HOME" ]; then
	# Prefer the *workspace* checkout: that is the tree every `west build`
	# renders support/openocd.cfg into. A checkout of this repo on its own
	# keeps a stale copy (it is only rewritten when it is the configured
	# module), and a stale cfg is what made the probe look broken before
	# (15 MHz + v1 HID backend).
	for cand in "${AGM_WORKSPACE:-$HOME/zephyrproject}/modules/hal_ag32" \
		    "$HOME/zephyr-hal-ag32" "$SELF_DIR/.."; do
		# Find the module tree by a file that is always in it: the rendered
		# board cfg is a *build* artifact now  and lives in
		# <build>/logic/openocd.cfg, not in the source tree.
		if [ -f "$cand/zephyr/module.yml" ]; then
			AGM_HOME="$(cd "$cand" && pwd)"
			break
		fi
	done
fi

# The cfg: an explicit override, then the build dir (where the build renders
# it), then the previously source-tree copy.
SUPPORT_CFG="${AGM_OPENOCD_CFG:-}"
[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG="${AGM_BUILD_DIR:+${AGM_BUILD_DIR}/logic/openocd.cfg}"
[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG=$(ls -t "${AGM_WORKSPACE:-$HOME/zephyrproject}"/build*/logic/openocd.cfg /tmp/b_*/logic/openocd.cfg 2>/dev/null | head -1)
[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG="$AGM_HOME/boards/agm/$BOARD/support/openocd.cfg"
WARMUP="$AGM_HOME/tools/openocd_warmup.py"

# The board cfg sources the SDK's agrv2k.cfg. A capture-only run (no firmware
# argument) needs nothing from the SDK -- the reset and the reader are all it
# does -- so fall back to this module's own SDK-free target cfg when the SDK
# is not installed. Flashing still needs the SDK (its `agrv` flash driver),
# which is why the fallback is only taken for the no-firmware case.
MINIMAL_CFG="$AGM_HOME/boards/agm/agrv2k/shared/support/agrv2k-minimal.cfg"
SDK_CFG="$AGRV_SDK_PATH/platforms/AgRV/etc/agrv2k.cfg"
if [ -n "${AGM_OPENOCD_CFG:-}" ]; then
	OPENOCD_CFG=$AGM_OPENOCD_CFG
elif [ -z "${FW_FILE:-}" ] && [ ! -f "$SDK_CFG" ]; then
	OPENOCD_CFG=$MINIMAL_CFG
else
	OPENOCD_CFG=$SUPPORT_CFG
fi

echo "=== test_uart_capture.sh ==="
echo "board         = $BOARD"
echo "firmware      = ${FW_FILE:-<none: reset only>}"
echo "erase         = $DO_ERASE"
echo "timeout_sec   = $TIMEOUT"
echo "tty           = $TTY_DEV"
echo "capture_path  = $OUTFILE"
echo "adapter       = $AGRV_ADAPTER"
echo "hal_ag32 home  = ${AGM_HOME:-<not found>}"
echo "support_cfg   = $SUPPORT_CFG"
echo "openocd_cfg   = $OPENOCD_CFG   (the one actually passed to openocd)"

# ---------- preflight -------------------------------------------------------
if [ -z "${VIRTUAL_ENV:-}" ]; then
	echo "FATAL: no Zephyr virtualenv active - activate the repo venv first" >&2
	exit 3
fi
PYTHON=${PYTHON:-$VIRTUAL_ENV/bin/python3}

[ -x "$OPENOCD_CMD" ] || { echo "FATAL: openocd_cmd not found at $OPENOCD_CMD" >&2; exit 3; }
# Check the cfg that is actually used, not $SUPPORT_CFG: a capture-only run on a
# machine without the SDK uses agrv2k-minimal.cfg and then needs no rendered
# board cfg at all. (Checking $SUPPORT_CFG here used to make that documented
# fallback unreachable -- and, because the source-tree copy is a build artifact
# since then, it also turned "I cleaned /tmp" into a bare "not found".)
[ -f "$OPENOCD_CFG" ] || {
	echo "FATAL: openocd cfg not found at $OPENOCD_CFG" >&2
	echo "       the per-board cfg is a *build* artifact (<build>/logic/openocd.cfg," >&2
	echo "       rendered by configure_file(); the source tree keeps no copy). Point" >&2
	echo "       the script at one of these and re-run:" >&2
	echo "         AGM_BUILD_DIR=<any build dir of this board>  (e.g. /tmp/b_hello)" >&2
	echo "         AGM_OPENOCD_CFG=<path to an openocd.cfg>      (explicit override)" >&2
	echo "       or run a \`west build -b $BOARD ...\` once -- that renders it." >&2
	exit 3
}
if [ -n "$FW_FILE" ]; then
	[ -f "$FW_FILE" ] || { echo "FATAL: firmware not found: $FW_FILE" >&2; exit 3; }
fi
[ -e "$TTY_DEV" ] || { echo "FATAL: $TTY_DEV missing (probe unplugged, or cdc_acm not bound)" >&2; exit 3; }

# ---------- 1. probe warmup -------------------------------------------------
# On-board CMSIS-DAP FW 2.1.0 ignores GET_DESCRIPTOR(STRING) inside openocd's
# hard-coded 1 s libusb timeout; one slow pyusb descriptor read fixes it.
# There is deliberately NO detach_kernel_driver() in that script: detaching
# cdc_acm (intf 1+2) permanently removed /dev/ttyACM0.
if [ -f "$WARMUP" ]; then
	echo ""
	echo "=== 1. probe warmup ==="
	"$PYTHON" "$WARMUP" || echo "WARN: warmup failed (continuing)" >&2
else
	echo ""
	echo "WARN: warmup script not found at $WARMUP" >&2
fi

# ---------- 2. serial port setup -------------------------------------------
echo ""
echo "=== 2. $TTY_DEV setup ==="
stty -F "$TTY_DEV" 115200 cs8 -cstopb -parenb -crtscts raw 2>&1 || {
	echo "FATAL: stty failed on $TTY_DEV" >&2; exit 3; }
echo "stty OK: $(stty -F "$TTY_DEV" speed 2>&1)"

# Drop stale bytes from a previous boot, then start the reader BEFORE reset.
dd if="$TTY_DEV" of=/dev/null bs=64 count=1 iflag=nonblock 2>/dev/null
rm -f "$OUTFILE"

echo ""
echo "=== 3. reading $TTY_DEV in background (${TIMEOUT}s) ==="
( timeout "$TIMEOUT" cat "$TTY_DEV" > "$OUTFILE" 2>/dev/null ) &
READER_PID=$!
sleep 0.5

# ---------- 4. flash (optional) + reset run ---------------------------------
echo ""
# When iterating fast across samples (e.g. a 16-sample build/flash/capture
# batch), the on-board CMSIS-DAP probe + USB-CDC bridge haven't always
# settled from the previous boot by the time we kick off the next flash.
# Symptoms: "Error: Flash write data timed out after NNN ms." -- happens
# ~1 in 4 runs in batch mode, ~1 in 12 standalone. A 1 s settle before
# openocd starts and a longer 2 s gap between retries both make the
# failure drop to ~0. Keep this conservative; the cycle is human-paced.
if [ -n "$FW_FILE" ]; then
	echo "=== 4. openocd: erase fw region + write + verify + reset run ==="
	sleep 1
	OCD_ARGS=(-c "init" -c "halt")
	if [ "$DO_ERASE" -eq 1 ]; then
		OCD_ARGS+=(-c "flash erase_address $FW_LOAD_ADDR $FW_ERASE_LEN")
	fi
	OCD_ARGS+=(-c "flash write_image $FW_FILE $FW_LOAD_ADDR bin"
	           -c "verify_image $FW_FILE $FW_LOAD_ADDR bin"
	           -c "reset run" -c "shutdown")
else
	echo "=== 4. openocd: reset run (no flash) ==="
	OCD_ARGS=(-c "init" -c "reset run" -c "shutdown")
fi

export AGRV_ADAPTER

# The AgRV flash driver intermittently reports
#   "Error: Flash write data timed out after NNN ms."
# on the first program after an erase (seen ~1 in 6 runs), and the SWD
# examine itself flakes (2 runs in 6). The region is then
# left erased, so a retry of the whole session recovers; give up on
# anything else. AGM_OPENOCD_ATTEMPTS overrides the budget.
FLASH_ATTEMPTS=${AGM_OPENOCD_ATTEMPTS:-3}
attempt=1
while :; do
	OCD_OUT="$("$OPENOCD_CMD" -s "$PLATFORM_ETC" -f "$OPENOCD_CFG" "${OCD_ARGS[@]}" 2>&1)"
	OCD_RC=$?
	printf '%s\n' "$OCD_OUT" | tail -12

	if printf '%s' "$OCD_OUT" | grep -qE "Flash write data timed out|error writing to flash|Examination failed|stalled AP|dmstatus=0x0|Couldn't determine state|examine-end failed"; then
		if [ "$attempt" -lt "$FLASH_ATTEMPTS" ]; then
			echo "WARN: openocd session failed (attempt $attempt/$FLASH_ATTEMPTS) - retrying" >&2
			attempt=$((attempt + 1))
			sleep 2
			continue
		fi
		echo "FATAL: openocd session failed after $FLASH_ATTEMPTS attempts (raise AGM_OPENOCD_ATTEMPTS to retry more)" >&2
		exit 3
	fi

	if [ $OCD_RC -ne 0 ]; then
		# Some openocd builds exit non-zero for "shutdown command invoked".
		if printf '%s' "$OCD_OUT" | grep -q "shutdown command invoked"; then
			echo "(openocd rc=$OCD_RC is the harmless shutdown-status return)"
		else
			echo "WARN: openocd rc=$OCD_RC" >&2
		fi
	fi
	break
done

# ---------- 5. collect ------------------------------------------------------
wait "$READER_PID" 2>/dev/null || true

echo ""
echo "=== 5. captured $OUTFILE ==="
BYTES=$(wc -c < "$OUTFILE")
echo "bytes = $BYTES"
if [ "$BYTES" -eq 0 ]; then
	echo "RESULT: EMPTY — no PL011 traffic reached $TTY_DEV" >&2
	echo "  (CPU halted by a debugger? probe bridge/cdc_acm down? firmware not running?)" >&2
	exit 1
fi

echo "--- text view (first 1 KiB) ---"
head -c 1024 "$OUTFILE"
echo ""
echo "---"

# ---------- 6. heuristics ---------------------------------------------------
echo ""
echo "=== 6. analysis ==="
PRINTABLE=$(tr -cd '\11\12\15\40-\176' < "$OUTFILE" | wc -c)
P_RATIO=$(( PRINTABLE * 100 / BYTES ))
echo "printable_ratio = ${P_RATIO}%   (>=90% expected for clean 115200 8N1)"

if grep -q 'Booting Zephyr OS' "$OUTFILE"; then
	echo "RESULT: OK — Zephyr boot banner (printk + PL011 + pin + bridge all working)"
	exit 0
elif grep -q 'AgRV2K busy-path tick demo' "$OUTFILE"; then
	echo "RESULT: OK — tick_busy banner (continuous UART output verified)"
	exit 0
elif grep -q 'UART TX DMA' "$OUTFILE"; then
	echo "RESULT: OK — SDK example_uart DMA message (SDK TX path works)"
	exit 0
elif [ "$P_RATIO" -lt 70 ]; then
	echo "RESULT: GARBAGE (baud / framing mismatch — got ${P_RATIO}% printable)" >&2
	exit 2
fi

echo "RESULT: UNKNOWN — captured ${BYTES} B, ${P_RATIO}% printable; inspect manually."
exit 0
